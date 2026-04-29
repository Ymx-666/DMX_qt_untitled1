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
#include <QFile>
#include <QDateTime>
#include <QThread>
#include <QMutexLocker>
#include <QDataStream>
#include <QStorageInfo>
#include <QShortcut>
#include <QDockWidget> // 用于侧拉框
#include <QTextBrowser>// 用于显示日志文字
#include <QTime>       // 用于日志时间戳
#include <QDialog>
#include <QScrollArea>
#include <QKeyEvent>
#include <QPixmap>
#include <QStringList>
#include <QMouseEvent>
#include <QApplication>
#include <QDesktopWidget>

// =========================================================
// 【模块二配置】：硬件网络参数
// =========================================================
const QString DEVICE_IP = "192.168.4.1"; // 协议中指定的硬件 IP
const quint16 CMD_PORT_SEND = 5001;      // 控制命令消息端口
const quint16 CMD_PORT_REPLY = 5002;     // 采集设备返回消息端口

class RoiPopupDialog : public QDialog
{
public:
    explicit RoiPopupDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("ROI");
        resize(1000, 800);
        m_label = new QLabel();
        m_label->setBackgroundRole(QPalette::Base);
        m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_label->setScaledContents(false);

        m_scroll = new QScrollArea(this);
        m_scroll->setWidget(m_label);
        m_scroll->setWidgetResizable(true);

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_scroll);
        setLayout(layout);
    }

    void setImage(const QImage &img)
    {
        m_img = img;
        m_scale = 1.0;
        refresh();
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!event) return;
        const int k = event->key();
        if (k == Qt::Key_Plus || k == Qt::Key_Equal) {
            m_scale *= 1.25;
            refresh();
            return;
        }
        if (k == Qt::Key_Minus || k == Qt::Key_Underscore) {
            m_scale /= 1.25;
            if (m_scale < 0.05) m_scale = 0.05;
            refresh();
            return;
        }
        if (k == Qt::Key_0) {
            m_scale = 1.0;
            refresh();
            return;
        }
        QDialog::keyPressEvent(event);
    }

private:
    void refresh()
    {
        if (m_img.isNull() || !m_label) return;
        QSize targetSize(
            qMax(1, (int)qRound(m_img.width() * m_scale)),
            qMax(1, (int)qRound(m_img.height() * m_scale))
        );
        QImage scaled = (m_scale == 1.0) ? m_img : m_img.scaled(targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
        m_label->setPixmap(QPixmap::fromImage(scaled));
        m_label->adjustSize();
    }

    QScrollArea *m_scroll = nullptr;
    QLabel *m_label = nullptr;
    QImage m_img;
    double m_scale = 1.0;
};

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

    connect(colorRoiView, &AIVideoWidget::clickedAt, this, [=](QPoint) {
        addLog("ROI", QString("RGB ROI 点击 (hasImg=%1 size=%2x%3)")
                   .arg(!m_lastColorRoi.isNull())
                   .arg(m_lastColorRoi.width())
                   .arg(m_lastColorRoi.height()),
               "#00AAAA");
        if (m_lastColorRoi.isNull()) {
            addLog("ROI", "RGB ROI 点击：当前没有可用ROI数据", "#F44336");
            if (ui->statusbar) ui->statusbar->showMessage("RGB ROI 暂无数据（请先点击上方全景条获取ROI）", 3000);
            return;
        }
        RoiPopupDialog *dlg = new RoiPopupDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        dlg->setWindowFlags(dlg->windowFlags() | Qt::Window);
        dlg->setWindowTitle("RGB ROI  (+/- 缩放, 0 重置)");
        dlg->setImage(m_lastColorRoi);
        if (QApplication::desktop()) {
            const QRect g = QApplication::desktop()->availableGeometry(this);
            dlg->move(g.center() - QPoint(dlg->width() / 2, dlg->height() / 2));
        }
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    connect(thermalRoiView, &AIVideoWidget::clickedAt, this, [=](QPoint) {
        addLog("ROI", QString("BW ROI 点击 (hasImg=%1 size=%2x%3)")
                   .arg(!m_lastThermalRoi.isNull())
                   .arg(m_lastThermalRoi.width())
                   .arg(m_lastThermalRoi.height()),
               "#00AAAA");
        if (m_lastThermalRoi.isNull()) {
            addLog("ROI", "BW ROI 点击：当前没有可用ROI数据", "#F44336");
            if (ui->statusbar) ui->statusbar->showMessage("BW ROI 暂无数据（请先点击上方全景条获取ROI）", 3000);
            return;
        }
        RoiPopupDialog *dlg = new RoiPopupDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        dlg->setWindowFlags(dlg->windowFlags() | Qt::Window);
        dlg->setWindowTitle("BW ROI  (+/- 缩放, 0 重置)");
        dlg->setImage(m_lastThermalRoi);
        if (QApplication::desktop()) {
            const QRect g = QApplication::desktop()->availableGeometry(this);
            dlg->move(g.center() - QPoint(dlg->width() / 2, dlg->height() / 2));
        }
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });

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
        const double displayAngle = m_zeroAngleInited ? toRelativeAngle(realAngle) : realAngle;
        radarView->setCurrentAngle(displayAngle);
        m_angleLabel->setText(QString("%1°").arg(displayAngle, 0, 'f', 2));
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

    m_colorThread = new VideoThread(0, this);
    m_thermalThread = new VideoThread(1, this);
    m_pathThread = new VideoThread(2, this);

    // 【新增】：接收子线程汇报的日志
    connect(m_colorThread, &VideoThread::logRequested, this, &MainWindow::addLog);
    connect(m_thermalThread, &VideoThread::logRequested, this, &MainWindow::addLog);
    connect(m_pathThread, &VideoThread::logRequested, this, &MainWindow::addLog);

    connect(m_colorThread, &VideoThread::frameCaptured, this, &MainWindow::onColorFrameReceived);
    connect(m_thermalThread, &VideoThread::thermalFrameCaptured, this, &MainWindow::onThermalFrameReceived);
    connect(m_colorThread, &VideoThread::roiCaptured, this, &MainWindow::onColorRoiCaptured);
    connect(m_thermalThread, &VideoThread::roiCaptured, this, &MainWindow::onThermalRoiCaptured);
    connect(m_colorThread, &VideoThread::panoramaSnapshotReady, this, &MainWindow::onRgbPanoramaSnapshotReady);
    connect(m_thermalThread, &VideoThread::panoramaSnapshotReady, this, &MainWindow::onBwPanoramaSnapshotReady);
    connect(m_pathThread, &VideoThread::pathReceived, this, &MainWindow::onPathReceived);

    m_pathThread->start();
    m_colorThread->start();
    m_thermalThread->start();

    connect(panoramaView, SIGNAL(angleSelected(double)), this, SLOT(onPanoramaClicked(double)));
    connect(thermalPanoramaView, SIGNAL(angleSelected(double)), this, SLOT(onPanoramaClicked(double)));
    connect(radarView, SIGNAL(sectorClicked(int)), this, SLOT(onRadarClicked(int)));

    initSimulatedTargets();
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event) {
        QWidget *w = childAt(event->pos());
        if (w && colorRoiView && (w == colorRoiView || colorRoiView->isAncestorOf(w))) {
            addLog("ROI", QString("RGB ROI click fallback (hasImg=%1 size=%2x%3)")
                       .arg(!m_lastColorRoi.isNull())
                       .arg(m_lastColorRoi.width())
                       .arg(m_lastColorRoi.height()),
                   "#00AAAA");
            if (!m_lastColorRoi.isNull()) {
                RoiPopupDialog *dlg = new RoiPopupDialog(this);
                dlg->setAttribute(Qt::WA_DeleteOnClose, true);
                dlg->setWindowFlags(dlg->windowFlags() | Qt::Window);
                dlg->setWindowTitle("RGB ROI  (+/- 缩放, 0 重置)");
                dlg->setImage(m_lastColorRoi);
                if (QApplication::desktop()) {
                    const QRect g = QApplication::desktop()->availableGeometry(this);
                    dlg->move(g.center() - QPoint(dlg->width() / 2, dlg->height() / 2));
                }
                dlg->show();
                dlg->raise();
                dlg->activateWindow();
            }
            return;
        }
        if (w && thermalRoiView && (w == thermalRoiView || thermalRoiView->isAncestorOf(w))) {
            addLog("ROI", QString("BW ROI click fallback (hasImg=%1 size=%2x%3)")
                       .arg(!m_lastThermalRoi.isNull())
                       .arg(m_lastThermalRoi.width())
                       .arg(m_lastThermalRoi.height()),
                   "#00AAAA");
            if (!m_lastThermalRoi.isNull()) {
                RoiPopupDialog *dlg = new RoiPopupDialog(this);
                dlg->setAttribute(Qt::WA_DeleteOnClose, true);
                dlg->setWindowFlags(dlg->windowFlags() | Qt::Window);
                dlg->setWindowTitle("BW ROI  (+/- 缩放, 0 重置)");
                dlg->setImage(m_lastThermalRoi);
                if (QApplication::desktop()) {
                    const QRect g = QApplication::desktop()->availableGeometry(this);
                    dlg->move(g.center() - QPoint(dlg->width() / 2, dlg->height() / 2));
                }
                dlg->show();
                dlg->raise();
                dlg->activateWindow();
            }
            return;
        }
    }
    QMainWindow::mousePressEvent(event);
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
        type.contains("ROI") ||
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

double MainWindow::toRelativeAngle(double rawAngleDeg)
{
    double a = rawAngleDeg;
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    if (!m_zeroAngleInited) return a;
    double r = a - m_zeroAngleRaw;
    while (r < 0.0) r += 360.0;
    while (r >= 360.0) r -= 360.0;
    return r;
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

static bool writeBmp24FromImage(const QString &outPath, int fullW, int fullH, const QImage &srcImg, QString *errMsg)
{
    if (srcImg.isNull()) {
        if (errMsg) *errMsg = "没有可保存的RGB数据";
        return false;
    }
    QImage img = srcImg;
    if (img.width() != fullW || img.height() != fullH) {
        img = img.scaled(fullW, fullH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    img = img.convertToFormat(QImage::Format_RGB888);

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

    if (!f.resize(fileSize)) {
        if (errMsg) *errMsg = "预分配失败";
        f.close();
        f.remove();
        return false;
    }

    for (int y = 0; y < fullH; ++y) {
        const qint64 pos = pixelOffset + (qint64)(fullH - 1 - y) * rowStride;
        if (!f.seek(pos)) {
            if (errMsg) *errMsg = "写入定位失败";
            f.close();
            f.remove();
            return false;
        }
        const uchar *line = img.constScanLine(y);
        const qint64 need = (qint64)rowStride;
        const qint64 wrote = f.write((const char*)line, need);
        if (wrote != need) {
            if (errMsg) *errMsg = "写入失败";
            f.close();
            f.remove();
            return false;
        }
    }

    f.close();
    return true;
}

static bool writeBmp8GrayFromImage(const QString &outPath, int fullW, int fullH, const QImage &srcImg, QString *errMsg)
{
    if (srcImg.isNull()) {
        if (errMsg) *errMsg = "没有可保存的BW数据";
        return false;
    }
    QImage img = srcImg;
    if (img.width() != fullW || img.height() != fullH) {
        img = img.scaled(fullW, fullH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    img = img.convertToFormat(QImage::Format_Indexed8);

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

    if (!f.resize(fileSize)) {
        if (errMsg) *errMsg = "预分配失败";
        f.close();
        f.remove();
        return false;
    }

    for (int y = 0; y < fullH; ++y) {
        const qint64 pos = pixelOffset + (qint64)(fullH - 1 - y) * rowStride;
        if (!f.seek(pos)) {
            if (errMsg) *errMsg = "写入定位失败";
            f.close();
            f.remove();
            return false;
        }
        const uchar *line = img.constScanLine(y);
        const qint64 need = (qint64)rowStride;
        const qint64 wrote = f.write((const char*)line, need);
        if (wrote != need) {
            if (errMsg) *errMsg = "写入失败";
            f.close();
            f.remove();
            return false;
        }
    }

    f.close();
    return true;
}

void MainWindow::onSaveFullPanoramaClicked()
{
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
        m_pendingSaveSnapshots = 2;
    }
    updateUiState();

    const QString rgbDir = "E:/.trae/program/DMX_qt/untitled1/data/RGB";
    const QString bwDir = "E:/.trae/program/DMX_qt/untitled1/data/BW";
    QDir().mkpath(rgbDir);
    QDir().mkpath(bwDir);

    const QString ts = QDateTime::currentDateTime().toString("yyyy_MM_dd_HH_mm_ss");
    const QString rgbOut = QDir(rgbDir).filePath(ts + "_rgb.bmp");
    const QString bwOut = QDir(bwDir).filePath(ts + "_bw.bmp");
    m_pendingSaveRgbPath = rgbOut;
    m_pendingSaveBwPath = bwOut;
    m_pendingSaveRgb = QImage();
    m_pendingSaveBw = QImage();

    {
        const int fullW = 65536;
        const int fullH = 4096;
        const qint64 rgbNeed = 54 + (qint64)fullW * fullH * 3;
        const qint64 bwNeed = (14 + 40 + 256 * 4) + (qint64)fullW * fullH;
        const qint64 need = rgbNeed + bwNeed + 64ll * 1024 * 1024;
        QStorageInfo st(QFileInfo(rgbOut).absolutePath());
        if (st.isValid() && st.isReady() && st.bytesAvailable() > 0 && st.bytesAvailable() < need) {
            {
                QMutexLocker locker(&m_fullSaveMutex);
                m_isSavingFullPanorama = false;
            }
            updateUiState();
            const QString msg = QString("磁盘剩余空间不足（need=%1MB free=%2MB）")
                .arg(need / (1024 * 1024))
                .arg(st.bytesAvailable() / (1024 * 1024));
            addLog("保存全图", "失败: " + msg, "#F44336");
            if (ui->statusbar) ui->statusbar->showMessage("保存全图失败: " + msg, 5000);
            return;
        }
    }

    addLog("保存全图", QString("开始保存快照：%1").arg(ts), "#569CD6");
    if (ui->statusbar) ui->statusbar->showMessage("正在保存全景图快照...", 3000);

    if (m_colorThread) m_colorThread->requestPanoramaSnapshot();
    if (m_thermalThread) m_thermalThread->requestPanoramaSnapshot();
}

void MainWindow::onSaveFullPanoramaFinished(bool ok, const QString &msg, const QString &rgbPath, const QString &bwPath)
{
    {
        QMutexLocker locker(&m_fullSaveMutex);
        m_isSavingFullPanorama = false;
    }
    m_pendingSaveSnapshots = 0;
    m_pendingSaveRgb = QImage();
    m_pendingSaveBw = QImage();
    m_pendingSaveRgbPath.clear();
    m_pendingSaveBwPath.clear();
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

    m_zeroAngleInited = false;
    m_zeroAngleRaw = 0.0;
    m_lastColorRoi = QImage();
    m_lastThermalRoi = QImage();
    m_pendingRadarFeedback = false;
    m_pendingSaveSnapshots = 0;
    m_pendingSaveRgb = QImage();
    m_pendingSaveBw = QImage();
    m_pendingSaveRgbPath.clear();
    m_pendingSaveBwPath.clear();
    if(m_logBrowser) m_logBrowser->clear(); // 清空日志

    for(auto& target : m_simTargets) { target.isDetected = false; }
    radarView->setTargets(m_simTargets);

    {
        QMutexLocker locker(&m_fullSaveMutex);
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

    if (m_colorThread) m_colorThread->requestRoi(angle, 0);
    if (m_thermalThread) m_thermalThread->requestRoi(angle, 1);
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

void MainWindow::onColorFrameReceived(QImage img, double angleDeg)
{
    if (!m_zeroAngleInited) {
        m_zeroAngleInited = true;
        m_zeroAngleRaw = m_latestAngle;
    }
    if (!img.isNull() && img.width() > 0) {
        const int sliceW = img.width();
        const int segments = qMax(1, fullPanoramaImage.width() / sliceW);
        double a = angleDeg;
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
                m_rgbSegFilled = QBitArray(segments, false);
            }
            if (tileIndex >= 0 && tileIndex < m_rgbSegments) {
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
    updatePanoramaSliceByAngle(img, angleDeg, 0);

    if ((nowMs - m_lastDetectMs) >= 120) {
        m_lastDetectMs = nowMs;
        checkTargetDetection(angleDeg);
    }

    if (m_prevCheckAngle > 300.0 && angleDeg < 60.0) {
        bool needReset = false;
        for(auto& target : m_simTargets) {
            if(target.isDetected) { target.isDetected = false; needReset = true; }
        }
        if(needReset) radarView->setTargets(m_simTargets);
    }
    m_prevCheckAngle = angleDeg;
}

void MainWindow::onThermalFrameReceived(QImage img, double angleDeg)
{
    if (!m_zeroAngleInited) {
        m_zeroAngleInited = true;
        m_zeroAngleRaw = m_latestAngle;
    }
    if (!img.isNull() && img.width() > 0) {
        const int sliceW = img.width();
        const int segments = qMax(1, fullThermalPanoramaImage.width() / sliceW);
        double a = angleDeg;
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
                m_bwSegFilled = QBitArray(segments, false);
            }
            if (tileIndex >= 0 && tileIndex < m_bwSegments) {
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
    updatePanoramaSliceByAngle(img, angleDeg, 1);
}

void MainWindow::onPathReceived(const QString &type, const QString &path, const QString &sender)
{
    const QString t = type.trimmed().toUpper();
    if (t == "RGB") {
        if (m_colorThread) m_colorThread->enqueuePath(t, path, sender);
        return;
    }
    if (t == "BW" || t == "GRAY") {
        if (m_thermalThread) m_thermalThread->enqueuePath(t, path, sender);
        return;
    }
}

void MainWindow::onColorRoiCaptured(QImage img, int tag)
{
    if (img.isNull()) return;
    if (tag == 0) {
        m_lastColorRoi = img;
        colorRoiView->updateImage(img);
        return;
    }
    if (tag == 2) {
        radarFeedbackView->updateImage(img);
        return;
    }
    if (tag == 3) {
        QImage targetImg = img;
        QPainter p(&targetImg);
        p.setPen(QPen(Qt::red, 8));
        p.drawRect(targetImg.rect().adjusted(8, 8, -8, -8));
        p.setPen(Qt::green);
        p.setFont(QFont("Arial", 40, QFont::Bold));
        p.drawText(40, 80, QString("DETECTED: %1 deg").arg(m_pendingCaptureAngle));
        p.end();
        captureView->updateImage(targetImg);
        return;
    }
}

void MainWindow::onThermalRoiCaptured(QImage img, int tag)
{
    if (img.isNull()) return;
    if (tag == 1) {
        m_lastThermalRoi = img;
        thermalRoiView->updateImage(img);
        return;
    }
}

void MainWindow::onRgbPanoramaSnapshotReady(QImage img)
{
    if (img.isNull()) return;
    if (m_pendingSaveSnapshots < 0) return;
    bool stored = false;
    if (m_pendingSaveRgb.isNull()) {
        m_pendingSaveRgb = img;
        stored = true;
    }
    if (stored && m_pendingSaveSnapshots > 0) --m_pendingSaveSnapshots;
    if (m_pendingSaveSnapshots != 0) return;
    if (m_pendingSaveRgb.isNull() || m_pendingSaveBw.isNull()) return;
    m_pendingSaveSnapshots = -1;

    const QString rgbOut = m_pendingSaveRgbPath;
    const QString bwOut = m_pendingSaveBwPath;
    const QImage rgb = m_pendingSaveRgb;
    const QImage bw = m_pendingSaveBw;

    QObject *worker = new QObject();
    QThread *t = new QThread(this);
    worker->moveToThread(t);
    connect(t, &QThread::started, worker, [=]() {
        const int fullW = 65536;
        const int fullH = 4096;
        QString err;
        bool ok = true;
        if (ok && !writeBmp24FromImage(rgbOut, fullW, fullH, rgb, &err)) ok = false;
        if (ok && !writeBmp8GrayFromImage(bwOut, fullW, fullH, bw, &err)) ok = false;
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

void MainWindow::onBwPanoramaSnapshotReady(QImage img)
{
    if (img.isNull()) return;
    if (m_pendingSaveSnapshots < 0) return;
    bool stored = false;
    if (m_pendingSaveBw.isNull()) {
        m_pendingSaveBw = img;
        stored = true;
    }
    if (stored && m_pendingSaveSnapshots > 0) --m_pendingSaveSnapshots;
    if (m_pendingSaveSnapshots != 0) return;
    if (m_pendingSaveRgb.isNull() || m_pendingSaveBw.isNull()) return;
    onRgbPanoramaSnapshotReady(m_pendingSaveRgb);
}

void MainWindow::onRadarClicked(int angle)
{
    if (m_colorThread) m_colorThread->requestRoi(angle, 2);
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
                m_pendingCaptureAngle = target.angle;
                if (m_colorThread) m_colorThread->requestRoi(target.angle, 3);
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
