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
#include <QFile>
#include <QDateTime>
#include <QImageReader>
#include <QThread>
#include <QMutexLocker>
#include <QDataStream>
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
    m_logBrowser(nullptr)
{
    ui->setupUi(this);
    this->resize(1600, 900);
    m_perfTimer.start();
    m_isDeviceOpen = false;
    m_lastColorUiMs = 0;
    m_lastThermalUiMs = 0;
    m_lastDetectMs = 0;
    m_lastLogMs = 0;
    m_rgbSegments = 0;
    m_bwSegments = 0;
    m_isSavingFullPanorama = false;

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
    // 5. 共享文件路径接收引擎 (8001 端口)
    // ====================================================================
    m_imagePaths.clear();
    m_imagePathSet.clear();

    m_pathThread = new VideoThread(2, this);
    m_colorThread = new VideoThread(3, this);
    m_thermalThread = new VideoThread(4, this);

    connect(m_pathThread, &VideoThread::pageTablePathReceived, this, [=](QString path){
        if (!m_imagePathSet.contains(path)) {
            m_imagePathSet.insert(path);
            m_imagePaths.append(path);
            if (m_imagePathSet.size() > 6000) {
                m_imagePathSet.clear();
                m_imagePaths.clear();
            }
        }
    });

    // 【新增】：接收子线程汇报的日志
    connect(m_pathThread, &VideoThread::logRequested, this, &MainWindow::addLog);
    connect(m_colorThread, &VideoThread::logRequested, this, &MainWindow::addLog);
    connect(m_thermalThread, &VideoThread::logRequested, this, &MainWindow::addLog);

    connect(m_pathThread, &VideoThread::pathJobReceived, m_colorThread, &VideoThread::enqueuePathJob, Qt::QueuedConnection);
    connect(m_pathThread, &VideoThread::pathJobReceived, m_thermalThread, &VideoThread::enqueuePathJob, Qt::QueuedConnection);

    connect(m_colorThread, &VideoThread::frameCaptured, this, &MainWindow::onColorFrameReceived);
    connect(m_thermalThread, &VideoThread::thermalFrameCaptured, this, &MainWindow::onThermalFrameReceived);

    m_pathThread->start();
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
    if(m_pathThread) { m_pathThread->stop(); m_pathThread->wait(); }
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
    m_logBrowser->document()->setMaximumBlockCount(1200);
    dock->setWidget(m_logBrowser);

    addDockWidget(Qt::LeftDockWidgetArea, dock);
    m_mainToolBar->addAction(dock->toggleViewAction());
}

void MainWindow::addLog(const QString &type, const QString &msg, const QString &color)
{
    if (!m_logBrowser) return;
    const qint64 nowMs = m_perfTimer.isValid() ? m_perfTimer.elapsed() : 0;
    const bool important =
        type.contains("错误") ||
        type.contains("读取失败") ||
        type.contains("系统") ||
        type.startsWith("RX(");
    if (!important && (nowMs - m_lastLogMs) < 15) return;
    m_lastLogMs = nowMs;
    QString timeStr = QTime::currentTime().toString("HH:mm:ss.zzz");
    const QString safeType = type.toHtmlEscaped();
    const QString safeMsg = msg.toHtmlEscaped();
    QString html = QString("<font color='gray'>[%1]</font> <font color='%2'><b>[%3]</b></font> %4")
                    .arg(timeStr).arg(color).arg(safeType).arg(safeMsg);
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
    m_actSaveFullPanorama = new QAction("保存全图", this);
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
    m_mainToolBar->addAction(m_actSaveFullPanorama);

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
    connect(m_actSaveFullPanorama, &QAction::triggered, this, &MainWindow::onSaveFullPanoramaClicked);
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

    bool ready = false;
    {
        QMutexLocker locker(&m_fullSaveMutex);
        ready = (m_rgbSegments > 0 && m_bwSegments > 0
                 && m_rgbSegFilled.size() == m_rgbSegments
                 && m_bwSegFilled.size() == m_bwSegments
                 && m_rgbSegFilled.count(true) == m_rgbSegments
                 && m_bwSegFilled.count(true) == m_bwSegments);
    }
    if (m_actSaveFullPanorama) {
        m_actSaveFullPanorama->setEnabled(!m_isSavingFullPanorama && ready);
    }
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

static bool loadImageWithRetry(const QString &path, QImage *outImg, QString *errMsg)
{
    if (!outImg) return false;
    for (int i = 0; i < 6; ++i) {
        QFileInfo fi(path);
        if (!fi.exists()) {
            QThread::msleep(50);
            continue;
        }
        QImageReader reader(path);
        reader.setAutoDetectImageFormat(true);
        QImage img = reader.read();
        if (!img.isNull()) {
            const QSize srcSize = img.size();
            if (srcSize.width() == 4096 && srcSize.height() == 4096) {
                img = img.transformed(QTransform().rotate(-90), Qt::FastTransformation);
            }
            *outImg = img;
            return true;
        }
        QThread::msleep(50);
    }
    if (errMsg) *errMsg = "读取失败: " + path;
    return false;
}

static bool writeBmp24FromSegments(const QString &outPath, int fullW, int fullH, const QVector<QString> &segPaths, QString *errMsg)
{
    if (segPaths.isEmpty()) {
        if (errMsg) *errMsg = "没有可保存的分片数据";
        return false;
    }
    const int segCount = segPaths.size();
    if (fullW % segCount != 0) {
        if (errMsg) *errMsg = "分片数量与目标宽度不匹配";
        return false;
    }
    const int segW = fullW / segCount;
    const int bytesPerPixel = 3;
    const int rowStride = fullW * bytesPerPixel;
    const int pixelOffset = 54;
    const qint64 imageSize = (qint64)rowStride * fullH;
    const qint64 fileSize = pixelOffset + imageSize;

    QFile f(outPath);
    if (!f.open(QIODevice::ReadWrite)) {
        if (errMsg) *errMsg = "无法创建文件: " + outPath;
        return false;
    }

    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0x4D42);
    ds << quint32((quint32)fileSize);
    ds << quint16(0) << quint16(0);
    ds << quint32(pixelOffset);
    ds << quint32(40);
    ds << qint32(fullW);
    ds << qint32(fullH);
    ds << quint16(1);
    ds << quint16(24);
    ds << quint32(0);
    ds << quint32((quint32)imageSize);
    ds << qint32(2835) << qint32(2835);
    ds << quint32(0) << quint32(0);
    f.resize(fileSize);

    for (int seg = 0; seg < segCount; ++seg) {
        const QString p = segPaths.at(seg);
        if (p.isEmpty()) {
            if (errMsg) *errMsg = "分片路径为空，无法保存";
            return false;
        }
        QImage img;
        QString loadErr;
        if (!loadImageWithRetry(p, &img, &loadErr)) {
            if (errMsg) *errMsg = loadErr;
            return false;
        }
        if (img.width() != segW || img.height() != fullH) {
            img = img.scaled(segW, fullH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        }
        img = img.convertToFormat(QImage::Format_RGB888);
        const qint64 xOffset = (qint64)seg * segW * bytesPerPixel;
        for (int y = 0; y < fullH; ++y) {
            const qint64 pos = pixelOffset + (qint64)(fullH - 1 - y) * rowStride + xOffset;
            if (!f.seek(pos)) {
                if (errMsg) *errMsg = "写入定位失败";
                return false;
            }
            const uchar *line = img.constScanLine(y);
            const qint64 need = (qint64)segW * bytesPerPixel;
            const qint64 wrote = f.write((const char*)line, need);
            if (wrote != need) {
                if (errMsg) *errMsg = "写入失败";
                return false;
            }
        }
    }
    f.close();
    return true;
}

static bool writeBmp8GrayFromSegments(const QString &outPath, int fullW, int fullH, const QVector<QString> &segPaths, QString *errMsg)
{
    if (segPaths.isEmpty()) {
        if (errMsg) *errMsg = "没有可保存的分片数据";
        return false;
    }
    const int segCount = segPaths.size();
    if (fullW % segCount != 0) {
        if (errMsg) *errMsg = "分片数量与目标宽度不匹配";
        return false;
    }
    const int segW = fullW / segCount;
    const int rowStride = fullW;
    const int pixelOffset = 14 + 40 + 256 * 4;
    const qint64 imageSize = (qint64)rowStride * fullH;
    const qint64 fileSize = pixelOffset + imageSize;

    QFile f(outPath);
    if (!f.open(QIODevice::ReadWrite)) {
        if (errMsg) *errMsg = "无法创建文件: " + outPath;
        return false;
    }

    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0x4D42);
    ds << quint32((quint32)fileSize);
    ds << quint16(0) << quint16(0);
    ds << quint32(pixelOffset);
    ds << quint32(40);
    ds << qint32(fullW);
    ds << qint32(fullH);
    ds << quint16(1);
    ds << quint16(8);
    ds << quint32(0);
    ds << quint32((quint32)imageSize);
    ds << qint32(2835) << qint32(2835);
    ds << quint32(256) << quint32(0);
    for (int i = 0; i < 256; ++i) {
        ds << quint8(i) << quint8(i) << quint8(i) << quint8(0);
    }
    f.resize(fileSize);

    for (int seg = 0; seg < segCount; ++seg) {
        const QString p = segPaths.at(seg);
        if (p.isEmpty()) {
            if (errMsg) *errMsg = "分片路径为空，无法保存";
            return false;
        }
        QImage img;
        QString loadErr;
        if (!loadImageWithRetry(p, &img, &loadErr)) {
            if (errMsg) *errMsg = loadErr;
            return false;
        }
        if (img.width() != segW || img.height() != fullH) {
            img = img.scaled(segW, fullH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        }
        img = img.convertToFormat(QImage::Format_Indexed8);
        const qint64 xOffset = (qint64)seg * segW;
        for (int y = 0; y < fullH; ++y) {
            const qint64 pos = pixelOffset + (qint64)(fullH - 1 - y) * rowStride + xOffset;
            if (!f.seek(pos)) {
                if (errMsg) *errMsg = "写入定位失败";
                return false;
            }
            const uchar *line = img.constScanLine(y);
            const qint64 need = (qint64)segW;
            const qint64 wrote = f.write((const char*)line, need);
            if (wrote != need) {
                if (errMsg) *errMsg = "写入失败";
                return false;
            }
        }
    }
    f.close();
    return true;
}

void MainWindow::onSaveFullPanoramaClicked()
{
    QVector<QString> rgbPaths;
    QVector<QString> bwPaths;
    int rgbSeg = 0;
    int bwSeg = 0;
    {
        QMutexLocker locker(&m_fullSaveMutex);
        if (m_isSavingFullPanorama) return;
        const bool ready = (m_rgbSegments > 0 && m_bwSegments > 0
                            && m_rgbSegFilled.size() == m_rgbSegments
                            && m_bwSegFilled.size() == m_bwSegments
                            && m_rgbSegFilled.count(true) == m_rgbSegments
                            && m_bwSegFilled.count(true) == m_bwSegments);
        if (!ready) {
            if (ui->statusbar) ui->statusbar->showMessage("全景图尚未拼接完成，无法保存全图", 3000);
            addLog("保存全图", "拦截：全景图未拼接完成", "#F44336");
            return;
        }
        m_isSavingFullPanorama = true;
        rgbPaths = m_rgbSegPaths;
        bwPaths = m_bwSegPaths;
        rgbSeg = m_rgbSegments;
        bwSeg = m_bwSegments;
    }
    updateUiState();

    const QString rgbDir = "E:/.trae/program/DMX_qt/untitled1/data/RGB";
    const QString bwDir = "E:/.trae/program/DMX_qt/untitled1/data/BW";
    QDir().mkpath(rgbDir);
    QDir().mkpath(bwDir);

    const QString ts = QDateTime::currentDateTime().toString("yyyy_MM_dd_HH_mm_ss");
    const QString rgbOut = QDir(rgbDir).filePath(ts + "_rgb.bmp");
    const QString bwOut = QDir(bwDir).filePath(ts + "_bw.bmp");

    addLog("保存全图", QString("开始保存快照：%1").arg(ts), "#569CD6");
    if (ui->statusbar) ui->statusbar->showMessage("正在保存全景图快照...", 3000);

    QObject *worker = new QObject();
    QThread *t = new QThread(this);
    worker->moveToThread(t);
    connect(t, &QThread::started, worker, [=]() {
        const int fullW = 65536;
        const int fullH = 4096;
        QString err;
        bool ok = true;
        if (rgbSeg <= 0 || bwSeg <= 0) ok = false;
        if (ok && rgbPaths.size() != rgbSeg) ok = false;
        if (ok && bwPaths.size() != bwSeg) ok = false;
        if (ok && !writeBmp24FromSegments(rgbOut, fullW, fullH, rgbPaths, &err)) ok = false;
        if (ok && !writeBmp8GrayFromSegments(bwOut, fullW, fullH, bwPaths, &err)) ok = false;
        QMetaObject::invokeMethod(this, "onSaveFullPanoramaFinished", Qt::QueuedConnection,
                                  Q_ARG(bool, ok),
                                  Q_ARG(QString, ok ? QString("保存成功") : err),
                                  Q_ARG(QString, rgbOut),
                                  Q_ARG(QString, bwOut));
        t->quit();
    });
    connect(t, &QThread::finished, worker, &QObject::deleteLater);
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MainWindow::onSaveFullPanoramaFinished(bool ok, const QString &msg, const QString &rgbPath, const QString &bwPath)
{
    {
        QMutexLocker locker(&m_fullSaveMutex);
        m_isSavingFullPanorama = false;
    }
    updateUiState();
    if (ok) {
        addLog("保存全图", "完成: " + rgbPath, "#6A9955");
        addLog("保存全图", "完成: " + bwPath, "#6A9955");
        if (ui->statusbar) ui->statusbar->showMessage("保存全图成功", 4000);
        return;
    }
    addLog("保存全图", "失败: " + msg, "#F44336");
    if (ui->statusbar) ui->statusbar->showMessage("保存全图失败: " + msg, 5000);
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

    {
        QMutexLocker locker(&m_fullSaveMutex);
        m_rgbSegPaths.clear();
        m_bwSegPaths.clear();
        m_rgbSegFilled.clear();
        m_bwSegFilled.clear();
        m_rgbSegments = 0;
        m_bwSegments = 0;
    }
    updateUiState();

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

void MainWindow::updatePanoramaSliceByAngle(const QImage &frame, double angleDeg, int type)
{
    if (frame.isNull()) return;
    QImage *panoramaCanvas = (type == 0) ? &fullPanoramaImage : &fullThermalPanoramaImage;

    const int sliceW = frame.width();
    if (sliceW <= 0) return;
    int segments = panoramaCanvas->width() / sliceW;
    if (segments <= 0) segments = 1;
    double a = angleDeg;
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    int tileIndex = (int)(a / 360.0 * segments);
    if (tileIndex < 0) tileIndex = 0;
    if (tileIndex >= segments) tileIndex = segments - 1;
    if (type == 1) {
        tileIndex = (tileIndex + (segments / 2)) % segments;
    }
    const int ui_StartX = tileIndex * sliceW;

    QPainter p(panoramaCanvas);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.drawImage(ui_StartX, 0, frame.height() == 240 ? frame : frame.scaled(sliceW, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    p.end();
}

void MainWindow::onColorFrameReceived(QImage img, const QString &path)
{
    if (!path.isEmpty() && !img.isNull() && img.width() > 0) {
        const int sliceW = img.width();
        const int segments = qMax(1, fullPanoramaImage.width() / sliceW);
        double a = m_latestAngle;
        while (a < 0.0) a += 360.0;
        while (a >= 360.0) a -= 360.0;
        int tileIndex = (int)(a / 360.0 * segments);
        if (tileIndex < 0) tileIndex = 0;
        if (tileIndex >= segments) tileIndex = segments - 1;

        bool shouldUpdateUi = false;
        {
            QMutexLocker locker(&m_fullSaveMutex);
            const bool wasAllReady = (m_rgbSegments > 0 && m_bwSegments > 0
                                      && m_rgbSegFilled.size() == m_rgbSegments
                                      && m_bwSegFilled.size() == m_bwSegments
                                      && m_rgbSegFilled.count(true) == m_rgbSegments
                                      && m_bwSegFilled.count(true) == m_bwSegments);
            if (m_rgbSegments != segments) {
                m_rgbSegments = segments;
                m_rgbSegPaths = QVector<QString>(segments);
                m_rgbSegFilled = QBitArray(segments, false);
            }
            if (tileIndex >= 0 && tileIndex < m_rgbSegments) {
                m_rgbSegPaths[tileIndex] = path;
                m_rgbSegFilled.setBit(tileIndex, true);
            }
            const bool isAllReady = (m_rgbSegments > 0 && m_bwSegments > 0
                                     && m_rgbSegFilled.size() == m_rgbSegments
                                     && m_bwSegFilled.size() == m_bwSegments
                                     && m_rgbSegFilled.count(true) == m_rgbSegments
                                     && m_bwSegFilled.count(true) == m_bwSegments);
            shouldUpdateUi = (!wasAllReady && isAllReady);
        }
        if (shouldUpdateUi) updateUiState();
    }

    const qint64 nowMs = m_perfTimer.isValid() ? m_perfTimer.elapsed() : 0;
    if ((nowMs - m_lastColorUiMs) < 33) return;
    m_lastColorUiMs = nowMs;
    updatePanoramaSliceByAngle(img, m_latestAngle, 0);

    if ((nowMs - m_lastDetectMs) >= 120) {
        m_lastDetectMs = nowMs;
        checkTargetDetection(m_latestAngle);
    }

    if (m_prevCheckAngle > 300.0 && m_latestAngle < 60.0) {
        bool needReset = false;
        for(auto& target : m_simTargets) {
            if(target.isDetected) { target.isDetected = false; needReset = true; }
        }
        if(needReset) radarView->setTargets(m_simTargets);
    }
    m_prevCheckAngle = m_latestAngle;
}

void MainWindow::onThermalFrameReceived(QImage img, const QString &path)
{
    if (!path.isEmpty() && !img.isNull() && img.width() > 0) {
        const int sliceW = img.width();
        const int segments = qMax(1, fullThermalPanoramaImage.width() / sliceW);
        double a = m_latestAngle;
        while (a < 0.0) a += 360.0;
        while (a >= 360.0) a -= 360.0;
        int tileIndex = (int)(a / 360.0 * segments);
        if (tileIndex < 0) tileIndex = 0;
        if (tileIndex >= segments) tileIndex = segments - 1;
        tileIndex = (tileIndex + (segments / 2)) % segments;

        bool shouldUpdateUi = false;
        {
            QMutexLocker locker(&m_fullSaveMutex);
            const bool wasAllReady = (m_rgbSegments > 0 && m_bwSegments > 0
                                      && m_rgbSegFilled.size() == m_rgbSegments
                                      && m_bwSegFilled.size() == m_bwSegments
                                      && m_rgbSegFilled.count(true) == m_rgbSegments
                                      && m_bwSegFilled.count(true) == m_bwSegments);
            if (m_bwSegments != segments) {
                m_bwSegments = segments;
                m_bwSegPaths = QVector<QString>(segments);
                m_bwSegFilled = QBitArray(segments, false);
            }
            if (tileIndex >= 0 && tileIndex < m_bwSegments) {
                m_bwSegPaths[tileIndex] = path;
                m_bwSegFilled.setBit(tileIndex, true);
            }
            const bool isAllReady = (m_rgbSegments > 0 && m_bwSegments > 0
                                     && m_rgbSegFilled.size() == m_rgbSegments
                                     && m_bwSegFilled.size() == m_bwSegments
                                     && m_rgbSegFilled.count(true) == m_rgbSegments
                                     && m_bwSegFilled.count(true) == m_bwSegments);
            shouldUpdateUi = (!wasAllReady && isAllReady);
        }
        if (shouldUpdateUi) updateUiState();
    }

    const qint64 nowMs = m_perfTimer.isValid() ? m_perfTimer.elapsed() : 0;
    if ((nowMs - m_lastThermalUiMs) < 33) return;
    m_lastThermalUiMs = nowMs;
    updatePanoramaSliceByAngle(img, m_latestAngle, 1);
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
