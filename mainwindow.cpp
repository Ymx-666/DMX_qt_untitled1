#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDebug>
#include <QPainter>
#include <QtMath>
#include <QTimer>
#include <QDir>
#include <QFileInfoList>
#include <QShortcut>
#include <QDockWidget> // 用于侧拉框
#include <QTextBrowser>// 用于显示日志文字
#include <QTime>       // 用于日志时间戳
#include <opencv2/opencv.hpp>

// =========================================================
// 【模块二配置】：硬件网络参数
// =========================================================
const QString DEVICE_IP = "192.168.4.1"; // 协议中指定的硬件 IP
const quint16 CMD_PORT_SEND = 5001;      // 控制命令消息端口
const quint16 CMD_PORT_REPLY = 5002;     // 采集设备返回消息端口

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_isDeviceOpen(false),
    m_logBrowser(nullptr)
{
    ui->setupUi(this);
    this->resize(1600, 900);

    // ====================================================================
    // 1. 初始化顶部主控工具栏
    // ====================================================================
    createToolBar();

    // ====================================================================
    // 【新增】：初始化侧拉日志框
    // ====================================================================
    setupLogDock();

    // ====================================================================
    // 2. 初始化核心 UI 组件与布局
    // ====================================================================
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QGridLayout *layout = new QGridLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);

    panoramaView = new PanoramaWidget(this);
    thermalPanoramaView = new PanoramaWidget(this);
    colorRoiView = new AIVideoWidget(this);
    thermalRoiView = new AIVideoWidget(this);
    captureView = new AIVideoWidget(this);
    radarView = new RadarWidget(this);
    radarFeedbackView = new AIVideoWidget(this);

    panoramaView->setFixedHeight(150);
    thermalPanoramaView->setFixedHeight(150);

    m_angleLabel = new QLabel("0.00°", radarView);
    m_angleLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_angleLabel->setStyleSheet("color: #00FF00; font-family: 'Monospace'; font-size: 18px; font-weight: bold; background-color: rgba(0,0,0,120); padding: 4px; border-radius: 4px;");

    m_lapTimeLabel = new QLabel("Lap: -- s", radarView);
    m_lapTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_lapTimeLabel->setStyleSheet("color: #FFA500; font-family: 'Monospace'; font-size: 14px; font-weight: bold; background-color: rgba(0,0,0,120); padding: 4px; border-radius: 4px;");

    QVBoxLayout *radarInternalLayout = new QVBoxLayout(radarView);
    radarInternalLayout->setContentsMargins(10, 10, 10, 10);
    radarInternalLayout->setSpacing(2);
    radarInternalLayout->addWidget(m_angleLabel, 0, Qt::AlignTop | Qt::AlignRight);
    radarInternalLayout->addWidget(m_lapTimeLabel, 0, Qt::AlignTop | Qt::AlignRight);
    radarInternalLayout->addStretch();
    radarView->setLayout(radarInternalLayout);

    QVBoxLayout *topPanoramasLayout = new QVBoxLayout();
    topPanoramasLayout->setSpacing(0);
    topPanoramasLayout->setContentsMargins(0,0,0,0);
    topPanoramasLayout->addWidget(panoramaView);
    topPanoramasLayout->addWidget(thermalPanoramaView);
    layout->addLayout(topPanoramasLayout, 0, 0, 1, 2);

    layout->addWidget(colorRoiView, 1, 0, 1, 1);
    layout->addWidget(thermalRoiView, 1, 1, 1, 1);

    QHBoxLayout *bottomLayout = new QHBoxLayout();
    bottomLayout->addWidget(captureView);
    bottomLayout->addWidget(radarView);
    bottomLayout->addWidget(radarFeedbackView);
    layout->addLayout(bottomLayout, 2, 0, 1, 2);

    // ====================================================================
    // 3. 全局缓存、转台驱动与快捷键初始化
    // ====================================================================
    fullPanoramaImage = QImage(8192, 240, QImage::Format_RGB32);
    fullPanoramaImage.fill(Qt::black);
    panoramaView->updateImage(fullPanoramaImage);

    fullThermalPanoramaImage = QImage(8192, 240, QImage::Format_RGB32);
    fullThermalPanoramaImage.fill(Qt::black);
    thermalPanoramaView->updateImage(fullThermalPanoramaImage);

    m_latestAngle = 0.0;
    m_prevCheckAngle = 0.0;

    QTimer *renderTimer = new QTimer(this);
    connect(renderTimer, &QTimer::timeout, this, [=]() {
        if (panoramaView) panoramaView->updateImage(fullPanoramaImage);
        if (thermalPanoramaView) thermalPanoramaView->updateImage(fullThermalPanoramaImage);
    });
    renderTimer->start(66);

    m_driver = new TurntableDriver(this);
    m_ctrlDialog = new TurntableControlDialog(m_driver, this);

    QShortcut *shortcutF5 = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(shortcutF5, &QShortcut::activated, this, &MainWindow::onClearUiClicked);
    QShortcut *shortcutCtrlR = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(shortcutCtrlR, &QShortcut::activated, this, &MainWindow::onClearUiClicked);

    connect(m_driver, &TurntableDriver::angleUpdated, this, [=](double realAngle){
        m_latestAngle = realAngle;
        radarView->setCurrentAngle(realAngle);
        m_angleLabel->setText(QString("%1°").arg(realAngle, 0, 'f', 2));
    });

    connect(m_driver, &TurntableDriver::lapTimeMeasured, this, [=](double lapTime){
        m_lapTimeLabel->setText(QString("Lap: %1 s").arg(lapTime, 0, 'f', 2));
    });

    // ====================================================================
    // 4. 【模块二】：UDP 指令控制引擎初始化 (5001发 / 5002收)
    // ====================================================================
    m_cmdSocket = new QUdpSocket(this);
    m_replySocket = new QUdpSocket(this);

    addLog("系统", QString("指令控制准备就绪，目标: %1").arg(DEVICE_IP), "#569CD6");

    // 绑定 5002 端口监听硬件返回值
    if (m_replySocket->bind(QHostAddress::AnyIPv4, CMD_PORT_REPLY, QUdpSocket::ShareAddress)) {
        qDebug() << ">>> [UDP系统] 成功绑定本地 5002 端口，监听设备回复...";
        addLog("系统", "成功绑定 5002 端口，监听设备应答", "#6A9955");
    } else {
        qDebug() << ">>> [UDP系统] 错误：无法绑定 5002 端口！";
        addLog("系统", "错误：无法绑定 5002 端口", "#F44336");
    }

    // 收到数据时触发解析
    connect(m_replySocket, &QUdpSocket::readyRead, this, &MainWindow::onCommandReplyReceived);

    // ====================================================================
    // 5. 双光网络接收引擎与调度分配 (8001/8002/8003 端口)
    // ====================================================================
    m_imagePaths.clear();

    m_colorThread = new VideoThread(0, this);
    m_thermalThread = new VideoThread(1, this);

    connect(m_colorThread, &VideoThread::pageTablePathReceived, this, [=](QString path){
        if(!m_imagePaths.contains(path)) m_imagePaths.append(path);
    });

    // 【新增】：接收子线程汇报的日志
    connect(m_colorThread, &VideoThread::logRequested, this, &MainWindow::addLog);

    connect(m_colorThread, &VideoThread::frameCaptured, this, &MainWindow::onColorFrameReceived);
    connect(m_colorThread, &VideoThread::thermalFrameCaptured, this, &MainWindow::onThermalFrameReceived);
    connect(m_thermalThread, &VideoThread::frameCaptured, this, &MainWindow::onThermalFrameReceived);

    m_colorThread->start();
    m_thermalThread->start();

    connect(panoramaView, SIGNAL(angleSelected(double)), this, SLOT(onPanoramaClicked(double)));
    connect(thermalPanoramaView, SIGNAL(angleSelected(double)), this, SLOT(onPanoramaClicked(double)));
    connect(radarView, SIGNAL(sectorClicked(int)), this, SLOT(onRadarClicked(int)));

    initSimulatedTargets();
}

MainWindow::~MainWindow()
{
    if(m_driver) { m_driver->stop(); m_driver->closePort(); }
    if(m_colorThread) { m_colorThread->stop(); m_colorThread->wait(); }
    if(m_thermalThread) { m_thermalThread->stop(); m_thermalThread->wait(); }
    delete ui;
}

// ====================================================================
// 【新增】：日志模块实现
// ====================================================================
void MainWindow::setupLogDock()
{
    QDockWidget *dock = new QDockWidget("系统通信日志", this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_logBrowser = new QTextBrowser(dock);
    m_logBrowser->setStyleSheet("background-color: #1E1E1E; color: #D4D4D4; font-family: 'Consolas', 'Monospace'; font-size: 10pt;");
    dock->setWidget(m_logBrowser);

    addDockWidget(Qt::LeftDockWidgetArea, dock);
    m_mainToolBar->addAction(dock->toggleViewAction());
}

void MainWindow::addLog(const QString &type, const QString &msg, const QString &color)
{
    if (!m_logBrowser) return;
    QString timeStr = QTime::currentTime().toString("HH:mm:ss.zzz");
    QString html = QString("<font color='gray'>[%1]</font> <font color='%2'><b>[%3]</b></font> %4")
                    .arg(timeStr).arg(color).arg(type).arg(msg);
    m_logBrowser->append(html);
}

// ====================================================================
// 【模块二核心逻辑】：UDP 文本指令下发与监听
// ====================================================================
void MainWindow::sendCommand(const QString &cmd)
{
    QByteArray data = cmd.toUtf8();
    m_cmdSocket->writeDatagram(data, QHostAddress(DEVICE_IP), CMD_PORT_SEND);
    qDebug() << ">>> [UDP 发送] ->" << DEVICE_IP << ":" << CMD_PORT_SEND << "|" << cmd;
    addLog("指令下发 (5001)", cmd, "#569CD6");
}

void MainWindow::onCommandReplyReceived()
{
    while (m_replySocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_replySocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;

        m_replySocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        // 解析返回文本
        QString replyStr = QString::fromUtf8(datagram);
        qDebug() << "<<< [UDP 接收] <-" << sender.toString() << ":" << senderPort << "|" << replyStr;
        addLog("设备应答 (5002)", replyStr, "#6A9955");

        // 在左下角状态栏展示设备返回值（绿色勾勾强调）
        if(ui->statusbar) {
            ui->statusbar->showMessage(QString("✅ 硬件回传: %1").arg(replyStr), 5000);
        }
    }
}

// ====================================================================
// 工具栏构建与 UI 状态机
// ====================================================================
void MainWindow::createToolBar()
{
    m_mainToolBar = new QToolBar("主控工具栏", this);
    addToolBar(Qt::TopToolBarArea, m_mainToolBar);
    m_mainToolBar->setMovable(false);

    // 高度放宽到 40，确保文字不被裁剪
    m_mainToolBar->setFixedHeight(40);
    m_mainToolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // 去除Emoji，纯文本稳定渲染
    m_mainToolBar->setStyleSheet(
        "QToolBar { "
        "   background-color: #f0f0f0; "
        "   border-bottom: 1px solid #cccccc; "
        "   spacing: 10px; "
        "} "
        "QToolButton { "
        "   font-family: 'Microsoft YaHei'; "
        "   font-weight: bold; "
        "   font-size: 14px; "
        "   color: #000000; "
        "   padding: 6px 12px; "
        "   border: none; "
        "   border-radius: 4px; "
        "   background-color: transparent; "
        "} "
        "QToolButton:hover { "
        "   background-color: #dcdcdc; "
        "} "
        "QToolButton:disabled { "
        "   color: #a0a0a0; "
        "}"
    );

    m_actOpenDevice = new QAction("设备运行", this);
    m_actCloseDevice = new QAction("设备停止", this);
    m_actSavePng = new QAction("无损采集", this);
    m_actSaveJpg = new QAction("压缩采集", this);
    m_actSaveVideo = new QAction("实时网络采集", this);
    m_actStopCapture = new QAction("停止采集", this);
    m_actClearImage = new QAction("清除图像", this);
    m_actExit = new QAction("退出系统", this);

    m_mainToolBar->addAction(m_actOpenDevice);
    m_mainToolBar->addAction(m_actCloseDevice);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actSavePng);
    m_mainToolBar->addAction(m_actSaveJpg);
    m_mainToolBar->addAction(m_actSaveVideo);
    m_mainToolBar->addAction(m_actStopCapture);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actClearImage);

    QAction *actOpenTurntable = m_mainToolBar->addAction("转台控制");

    QWidget *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_mainToolBar->addWidget(spacer);
    m_mainToolBar->addAction(m_actExit);

    connect(m_actOpenDevice, &QAction::triggered, this, &MainWindow::onActionOpenDevice);
    connect(m_actCloseDevice, &QAction::triggered, this, &MainWindow::onActionCloseDevice);
    connect(m_actSavePng, &QAction::triggered, this, &MainWindow::onActionSavePng);
    connect(m_actSaveJpg, &QAction::triggered, this, &MainWindow::onActionSaveJpg);
    connect(m_actSaveVideo, &QAction::triggered, this, &MainWindow::onActionSaveVideo);
    connect(m_actStopCapture, &QAction::triggered, this, &MainWindow::onActionStopCapture);
    connect(m_actClearImage, &QAction::triggered, this, &MainWindow::onClearUiClicked);
    connect(actOpenTurntable, &QAction::triggered, this, [=](){ m_ctrlDialog->show(); });
    connect(m_actExit, &QAction::triggered, this, &MainWindow::close);

    updateUiState();
}

void MainWindow::updateUiState()
{
    m_actOpenDevice->setEnabled(!m_isDeviceOpen);
    m_actCloseDevice->setEnabled(m_isDeviceOpen);
    m_actSavePng->setEnabled(m_isDeviceOpen);
    m_actSaveJpg->setEnabled(m_isDeviceOpen);
    m_actSaveVideo->setEnabled(m_isDeviceOpen);
    m_actStopCapture->setEnabled(m_isDeviceOpen);
}

// ====================================================================
// 【模块二】：工具栏按钮调用 UDP 发送
// ====================================================================
void MainWindow::onActionOpenDevice()
{
    sendCommand("TG_OPEN_DEVICE;"); // 触发 UDP 发包
    m_isDeviceOpen = true;
    updateUiState();
    ui->statusbar->showMessage("正在向设备下发使能命令...", 2000);
}

void MainWindow::onActionCloseDevice()
{
    sendCommand("TG_CLOSE_DEVICE;"); // 触发 UDP 发包
    if (m_driver) m_driver->stop();  // 本地 UI 联动停止
    m_isDeviceOpen = false;
    updateUiState();
    ui->statusbar->showMessage("正在向设备下发停止命令...", 2000);
}

void MainWindow::onActionSavePng()
{
    sendCommand("TG_SAVE_PNG;");
    ui->statusbar->showMessage("指令已下发: 启动无损采集...", 2000);
}

void MainWindow::onActionSaveJpg()
{
    sendCommand("TG_SAVE_JPG;");
    ui->statusbar->showMessage("指令已下发: 启动压缩采集...", 2000);
}

void MainWindow::onActionSaveVideo()
{
    sendCommand("TG_SAVE_VIDEO;");
    ui->statusbar->showMessage("指令已下发: 启动实时网络视频采集...", 2000);
}

void MainWindow::onActionStopCapture()
{
    sendCommand("TG_STOP_CAPTURE;");
    ui->statusbar->showMessage("指令已下发: 停止图像采集！", 2000);
}

// ====================================================================
// UI 交互与渲染层 (保持不变)
// ====================================================================
void MainWindow::onClearUiClicked()
{
    fullPanoramaImage.fill(Qt::black);
    fullThermalPanoramaImage.fill(Qt::black);
    panoramaView->updateImage(fullPanoramaImage);
    thermalPanoramaView->updateImage(fullThermalPanoramaImage);

    QImage blackImg(1024, 2048, QImage::Format_RGB32);
    blackImg.fill(Qt::black);
    colorRoiView->updateImage(blackImg);
    thermalRoiView->updateImage(blackImg);
    captureView->updateImage(blackImg);
    radarFeedbackView->updateImage(blackImg);

    m_imagePaths.clear();
    if(m_logBrowser) m_logBrowser->clear(); // 清空日志

    for(auto& target : m_simTargets) { target.isDetected = false; }
    radarView->setTargets(m_simTargets);

    if(ui->statusbar) ui->statusbar->showMessage("界面缓存已清空！", 2000);
}

void MainWindow::onPanoramaClicked(double angle)
{
    panoramaView->setSelectedAngle(angle);
    thermalPanoramaView->setSelectedAngle(angle);

    int global_X = (int)((angle / 360.0) * 65536);
    int file_idx = 0;
    if (!m_imagePaths.isEmpty()) { file_idx = (global_X / 2048) % m_imagePaths.size(); }

    QImage colorImg = fetchRoiFromPageTable(file_idx, 0);
    if (!colorImg.isNull()) colorRoiView->updateImage(colorImg);

    QImage grayImg = fetchRoiFromPageTable(file_idx, 1);
    if (!grayImg.isNull()) thermalRoiView->updateImage(grayImg);
}

void MainWindow::updatePanoramaSlice(const QImage &frame, int fileIndex, int type)
{
    if (frame.isNull()) return;
    QImage *panoramaCanvas = (type == 0) ? &fullPanoramaImage : &fullThermalPanoramaImage;

    int tileIndex = fileIndex % 32;
    int ui_StartX = tileIndex * 256;
    int ui_Width = 256;

    QPainter p(panoramaCanvas);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.drawImage(ui_StartX, 0, frame.scaled(ui_Width, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    p.end();
}

void MainWindow::onColorFrameReceived(QImage img, int fileIndex)
{
    updatePanoramaSlice(img, fileIndex, 0);
    checkTargetDetection(m_latestAngle);

    if (m_prevCheckAngle > 300.0 && m_latestAngle < 60.0) {
        bool needReset = false;
        for(auto& target : m_simTargets) {
            if(target.isDetected) { target.isDetected = false; needReset = true; }
        }
        if(needReset) radarView->setTargets(m_simTargets);
    }
    m_prevCheckAngle = m_latestAngle;
}

void MainWindow::onThermalFrameReceived(QImage img, int fileIndex)
{
    updatePanoramaSlice(img, fileIndex, 1);
}

QImage MainWindow::fetchRoiFromPageTable(int file_idx, int type)
{
    if (m_imagePaths.isEmpty() || file_idx >= m_imagePaths.size() || file_idx < 0) return QImage();

    QString path = m_imagePaths.at(file_idx);

    if (type == 1) {
        cv::Mat mat = cv::imread(path.toStdString(), cv::IMREAD_GRAYSCALE);
        if(mat.empty()) return QImage();
        cv::resize(mat, mat, cv::Size(1024, 2048), 0, 0, cv::INTER_AREA);
        QImage img((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Indexed8);
        static QVector<QRgb> s_grayTable;
        if (s_grayTable.isEmpty()) {
            for (int i = 0; i < 256; ++i) s_grayTable.push_back(qRgb(i, i, i));
        }
        img.setColorTable(s_grayTable);
        return img.copy();
    } else {
        cv::Mat mat = cv::imread(path.toStdString(), cv::IMREAD_COLOR);
        if(mat.empty()) return QImage();
        cv::resize(mat, mat, cv::Size(1024, 2048), 0, 0, cv::INTER_AREA);
        cv::cvtColor(mat, mat, cv::COLOR_BGR2BGRA);
        QImage img((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_RGB32);
        return img.copy();
    }
}

void MainWindow::onRadarClicked(int angle)
{
    int global_X = (int)((angle / 360.0) * 65536);
    int file_idx = 0;

    if (!m_imagePaths.isEmpty()) { file_idx = (global_X / 2048) % m_imagePaths.size(); }

    QImage img = fetchRoiFromPageTable(file_idx, 0);
    if (!img.isNull()) radarFeedbackView->updateImage(img);
}

void MainWindow::checkTargetDetection(double currentAngle)
{
    bool targetsChanged = false;
    double tolerance = 2.0;

    for(auto& target : m_simTargets) {
        if (!target.isDetected) {
            double diff = qAbs(currentAngle - target.angle);
            if (diff > 180) diff = 360 - diff;

            if (diff < tolerance) {
                target.isDetected = true;
                targetsChanged = true;

                int global_X = (int)((target.angle / 360.0) * 65536);
                int file_idx = 0;

                if (!m_imagePaths.isEmpty()) { file_idx = (global_X / 2048) % m_imagePaths.size(); }

                QImage targetImg = fetchRoiFromPageTable(file_idx, 0);

                if (!targetImg.isNull()) {
                    QPainter p(&targetImg);
                    p.setPen(QPen(Qt::red, 8));
                    p.drawRect(targetImg.rect().adjusted(8,8,-8,-8));
                    p.setPen(Qt::green);
                    p.setFont(QFont("Arial", 40, QFont::Bold));
                    p.drawText(40, 80, QString("DETECTED: %1 deg").arg(target.angle));
                    p.end();

                    captureView->updateImage(targetImg);
                }
            }
        }
    }
    if(targetsChanged) radarView->setTargets(m_simTargets);
}

void MainWindow::initSimulatedTargets()
{
    m_simTargets.clear();
    m_simTargets.append(RadarTarget(45));
    m_simTargets.append(RadarTarget(135));
    m_simTargets.append(RadarTarget(240));
    m_simTargets.append(RadarTarget(315));
    radarView->setTargets(m_simTargets);
}
