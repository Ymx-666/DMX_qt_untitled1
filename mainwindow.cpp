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
#include <QDockWidget>
#include <QTextBrowser>
#include <QTime>
#include <QDialog>
#include <QScrollArea>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QSizeGrip>
#include <QPixmap>
#include <QStringList>
#include <QMouseEvent>
#include <QApplication>
#include <QDesktopWidget>
#include <QPointer>
#include <functional>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QColor>

#include "rawrecorder.h"
#include "asciipath.h"

static inline QString u8s(const char *s) { return QString::fromUtf8(s); }

// Linux 默认数据根：优先用 4TB 数据卷，避免写入系统盘 ~ 目录。
// 可用环境变量 DMX_DATA_ROOT 覆盖整个根，或用各自的 DMX_*_ROOT 单独覆盖。
static QString dataRootLinux()
{
    QString r = QString::fromLocal8Bit(qgetenv("DMX_DATA_ROOT"));
    if (!r.isEmpty()) return r;
    return QStringLiteral("/media/sht/6C3CCFC13CCF8494/data");
}

static QString resolveSaveRoot()
{
    QString r = QString::fromLocal8Bit(qgetenv("DMX_SAVE_ROOT"));
    if (!r.isEmpty()) return r;
#ifdef Q_OS_WIN
    return QStringLiteral("E:/.trae/program/DMX_qt/untitled1/data/SAVES");
#else
    return dataRootLinux() + QStringLiteral("/saves");
#endif
}

static QString resolveRecordRoot()
{
    QString r = QString::fromLocal8Bit(qgetenv("DMX_REC_ROOT"));
    if (!r.isEmpty()) return r;
#ifdef Q_OS_WIN
    return QStringLiteral("D:/DMX_data");
#else
    return dataRootLinux() + QStringLiteral("/recordings");
#endif
}

static QString resolveLogRoot()
{
    QString r = QString::fromLocal8Bit(qgetenv("DMX_LOG_ROOT"));
    if (!r.isEmpty()) return r;
#ifdef Q_OS_WIN
    return QStringLiteral("E:/.trae/program/DMX_qt/untitled1/data/logs");
#else
    return dataRootLinux() + QStringLiteral("/logs");
#endif
}


const QString DEVICE_IP = "192.168.4.1";
const quint16 CMD_PORT_SEND = 5001;
const quint16 CMD_PORT_REPLY = 5002;

class RoiPopupDialog : public QDialog
{
public:
    explicit RoiPopupDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("ROI");
        setSizeGripEnabled(true);
        resize(1000, 800);
        m_label = new QLabel();
        m_label->setBackgroundRole(QPalette::Base);
        m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_label->setScaledContents(false);
        m_label->setAlignment(Qt::AlignCenter);

        m_scroll = new QScrollArea(this);
        m_scroll->setWidget(m_label);
        // Keep the label at the pixmap's natural size so the scroll area can
        // scroll a zoomed image (magnifier), instead of stretching to fit.
        m_scroll->setWidgetResizable(false);
        m_scroll->setAlignment(Qt::AlignCenter);

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_scroll);
        setLayout(layout);
    }

    void setImage(const QImage &img)
    {
        if (img.isNull() || !m_label) return;
        m_label->setPixmap(QPixmap::fromImage(img));
        m_label->resize(img.size());

        // Mouse-centered zoom: after the re-scaled image arrives, scroll so the
        // pixel that was under the cursor stays under the cursor.
        if (m_anchorFx >= 0.0 && m_scroll) {
            const int newX = (int)(m_anchorFx * img.width()) - m_anchorViewportPos.x();
            const int newY = (int)(m_anchorFy * img.height()) - m_anchorViewportPos.y();
            if (m_scroll->horizontalScrollBar()) m_scroll->horizontalScrollBar()->setValue(newX);
            if (m_scroll->verticalScrollBar()) m_scroll->verticalScrollBar()->setValue(newY);
            m_anchorFx = -1.0;
            m_anchorFy = -1.0;
        }
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!event) return;
        if (event->isAutoRepeat()) return;
        const int k = event->key();
        if (k == Qt::Key_Plus || k == Qt::Key_Equal) {
            m_scale *= 1.25;
            if (m_onScale) m_onScale(m_scale);
            return;
        }
        if (k == Qt::Key_Minus || k == Qt::Key_Underscore) {
            m_scale /= 1.25;
            if (m_scale < 0.05) m_scale = 0.05;
            if (m_onScale) m_onScale(m_scale);
            return;
        }
        if (k == Qt::Key_0) {
            m_scale = 1.0;
            if (m_onScale) m_onScale(m_scale);
            return;
        }
        QDialog::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (!event || !m_scroll || !m_label) { QDialog::wheelEvent(event); return; }
        const int dy = event->angleDelta().y();
        if (dy == 0) { QDialog::wheelEvent(event); return; }

        // Record the image-space fraction under the cursor before zooming, so we
        // can scroll back to it once the re-scaled image arrives (see setImage).
        const QPoint vpPos = m_scroll->viewport()->mapFromGlobal(event->globalPos());
        const int hVal = m_scroll->horizontalScrollBar() ? m_scroll->horizontalScrollBar()->value() : 0;
        const int vVal = m_scroll->verticalScrollBar() ? m_scroll->verticalScrollBar()->value() : 0;
        const int lw = m_label->width();
        const int lh = m_label->height();
        if (lw > 0 && lh > 0) {
            m_anchorFx = (double)(hVal + vpPos.x()) / (double)lw;
            m_anchorFy = (double)(vVal + vpPos.y()) / (double)lh;
            m_anchorViewportPos = vpPos;
        }

        m_scale *= (dy > 0) ? 1.25 : (1.0 / 1.25);
        if (m_scale < 0.05) m_scale = 0.05;
        if (m_onScale) m_onScale(m_scale);
        event->accept();
    }

private:
    friend class MainWindow;
    void setScaleCallback(std::function<void(double)> cb) { m_onScale = std::move(cb); }

    QScrollArea *m_scroll = nullptr;
    QLabel *m_label = nullptr;
    double m_scale = 1.0;
    std::function<void(double)> m_onScale;
    double m_anchorFx = -1.0;
    double m_anchorFy = -1.0;
    QPoint m_anchorViewportPos;
};

RoiWorker::RoiWorker(QSharedPointer<PanoramaCache> cache, QObject *parent)
    : QObject(parent), m_cache(std::move(cache))
{
}

void RoiWorker::requestPreview(double angle)
{
    {
        QMutexLocker lk(&m_mtx);
        m_pendingPreview = true;
        m_previewAngle = angle;
    }
    schedule();
}

void RoiWorker::requestFullRgbScaled(double angle, double scale)
{
    {
        QMutexLocker lk(&m_mtx);
        m_pendingFullRgb = true;
        m_fullRgbAngle = angle;
        m_fullRgbScale = scale;
    }
    schedule();
}

void RoiWorker::requestFullBwScaled(double angle, double scale)
{
    {
        QMutexLocker lk(&m_mtx);
        m_pendingFullBw = true;
        m_fullBwAngle = angle;
        m_fullBwScale = scale;
    }
    schedule();
}

void RoiWorker::schedule()
{
    if (m_scheduled.testAndSetAcquire(0, 1)) {
        QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
    }
}

void RoiWorker::process()
{
    m_scheduled.storeRelease(0);

    bool doPreview = false;
    bool doFullRgb = false;
    bool doFullBw = false;
    double previewAngle = 0.0;
    double fullRgbAngle = 0.0;
    double fullBwAngle = 0.0;
    double fullRgbScale = 1.0;
    double fullBwScale = 1.0;

    {
        QMutexLocker lk(&m_mtx);
        doPreview = m_pendingPreview;
        doFullRgb = m_pendingFullRgb;
        doFullBw = m_pendingFullBw;
        previewAngle = m_previewAngle;
        fullRgbAngle = m_fullRgbAngle;
        fullBwAngle = m_fullBwAngle;
        fullRgbScale = m_fullRgbScale;
        fullBwScale = m_fullBwScale;
        m_pendingPreview = false;
        m_pendingFullRgb = false;
        m_pendingFullBw = false;
    }

    if (!m_cache) return;

    if (doPreview) {
        // Sliding-window ROI: take a one-tile-wide window centered on the angle
        // (windowW=0 -> sliceW), pixel-accurate and not tile-aligned.
        QImage rgb = m_cache->extractThumbRgbWindow(previewAngle, 0);
        QImage bw = m_cache->extractThumbBwWindow(previewAngle, 0);
        if (!bw.isNull() && bw.format() == QImage::Format_Indexed8) bw = bw.convertToFormat(QImage::Format_RGB32);
        emit previewReady(previewAngle, rgb, bw);
    }

    if (doFullRgb) {
        QImage rgb = m_cache->extractFullRgbWindow(fullRgbAngle, 0);
        if (rgb.isNull()) rgb = m_cache->extractThumbRgbWindow(fullRgbAngle, 0);
        if (!rgb.isNull()) {
            const qint64 maxPixels = 40ll * 1024 * 1024;
            const int maxDim = 16384;
            const double srcPixels = (double)rgb.width() * (double)rgb.height();
            double s = fullRgbScale;
            if (s < 0.05) s = 0.05;
            const double maxSByPixels = (srcPixels > 1.0) ? qSqrt((double)maxPixels / srcPixels) : 1.0;
            const double maxSByDim = (double)maxDim / (double)qMax(rgb.width(), rgb.height());
            double maxS = qMin(maxSByPixels, maxSByDim);
            if (maxS < 0.05) maxS = 0.05;
            if (s > maxS) s = maxS;
            if (s != 1.0) {
                QSize targetSize(qMax(1, (int)qRound(rgb.width() * s)), qMax(1, (int)qRound(rgb.height() * s)));
                rgb = rgb.scaled(targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
            }
            fullRgbScale = s;
        }
        emit fullScaledReady(false, fullRgbAngle, fullRgbScale, rgb);
    }

    if (doFullBw) {
        QImage bw = m_cache->extractFullBwWindow(fullBwAngle, 0);
        if (bw.isNull()) bw = m_cache->extractThumbBwWindow(fullBwAngle, 0);
        if (!bw.isNull() && bw.format() == QImage::Format_Indexed8) bw = bw.convertToFormat(QImage::Format_RGB32);
        if (!bw.isNull() && fullBwScale != 1.0) {
            const qint64 maxPixels = 40ll * 1024 * 1024;
            const int maxDim = 16384;
            const double srcPixels = (double)bw.width() * (double)bw.height();
            double s = fullBwScale;
            if (s < 0.05) s = 0.05;
            const double maxSByPixels = (srcPixels > 1.0) ? qSqrt((double)maxPixels / srcPixels) : 1.0;
            const double maxSByDim = (double)maxDim / (double)qMax(bw.width(), bw.height());
            double maxS = qMin(maxSByPixels, maxSByDim);
            if (maxS < 0.05) maxS = 0.05;
            if (s > maxS) s = maxS;
            QSize targetSize(qMax(1, (int)qRound(bw.width() * s)), qMax(1, (int)qRound(bw.height() * s)));
            bw = bw.scaled(targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
            fullBwScale = s;
        }
        emit fullScaledReady(true, fullBwAngle, fullBwScale, bw);
    }

    {
        QMutexLocker lk(&m_mtx);
        if (m_pendingPreview || m_pendingFullRgb || m_pendingFullBw) {
            schedule();
        }
    }
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_logBrowser(nullptr)
{
    ui->setupUi(this);
    this->resize(1600, 900);
    m_perfTimer.start();
    m_isDeviceOpen = false;
    m_lastDetectMs = 0;
    m_lastLogMs = 0;

    {
        const QDateTime now = QDateTime::currentDateTime();
        const QString logRoot = resolveLogRoot();
        char dateBuf[16];
        qsnprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
            now.date().year(), now.date().month(), now.date().day());
        const QString dateDir = QDir(logRoot).filePath(QString::fromLatin1(dateBuf));
        QDir().mkpath(dateDir);
        char fileBuf[32];
        qsnprintf(fileBuf, sizeof(fileBuf), "log_%02d-%02d-%02d.txt",
            now.time().hour(), now.time().minute(), now.time().second());
        const QString logPath = QDir(dateDir).filePath(QString::fromLatin1(fileBuf));
        m_logFile = new QFile(logPath);
        if (!m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete m_logFile;
            m_logFile = nullptr;
        }
    }
    m_renderPending.storeRelease(0);
    m_panoCache = QSharedPointer<PanoramaCache>(new PanoramaCache());
    {
        QImage rgb(8192, 240, QImage::Format_RGB32);
        rgb.fill(Qt::black);
        QImage bw(8192, 240, QImage::Format_Indexed8);
        QVector<QRgb> table;
        table.reserve(256);
        for (int i = 0; i < 256; ++i) table.push_back(qRgb(i, i, i));
        bw.setColorTable(table);
        bw.fill(0);
        m_uiThumbRgb = rgb;
        m_uiThumbBw = bw;
    }
    m_saveThread = new QThread(this);
    m_saveWorker = new PanoramaSaver(m_panoCache);
    m_saveWorker->moveToThread(m_saveThread);
    connect(this, &MainWindow::savePanoramaRequested, m_saveWorker, &PanoramaSaver::enqueueSave, Qt::QueuedConnection);
    connect(m_saveWorker, &PanoramaSaver::saveFinished, this, &MainWindow::onSaveFullPanoramaFinished, Qt::QueuedConnection);
    connect(m_saveThread, &QThread::finished, m_saveWorker, &QObject::deleteLater);
    m_saveThread->start();

    m_roiThread = new QThread(this);
    m_roiWorker = new RoiWorker(m_panoCache);
    m_roiWorker->moveToThread(m_roiThread);
    connect(m_roiThread, &QThread::finished, m_roiWorker, &QObject::deleteLater);
    connect(m_roiWorker, &RoiWorker::previewReady, this, &MainWindow::onThumbRoiReady, Qt::QueuedConnection);
    m_roiThread->start();

    m_recordThread = new QThread(this);
    m_recordWorker = new RawRecorder();
    m_recordWorker->moveToThread(m_recordThread);
    connect(m_recordWorker, &RawRecorder::logRequested, this, &MainWindow::addLog, Qt::QueuedConnection);
    connect(m_recordThread, &QThread::finished, m_recordWorker, &QObject::deleteLater);
    m_recordThread->start();

    createToolBar();
    setupLogDock();
    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
           QStringLiteral("BUILD exe=%1").arg(QCoreApplication::applicationFilePath()),
           QStringLiteral("#FFD54F"));
    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
           QStringLiteral("BUILD compiled=%1 %2").arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)),
           QStringLiteral("#FFD54F"));
    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
           QStringLiteral("BUILD saveRoot=%1 recRoot=%2").arg(resolveSaveRoot(), resolveRecordRoot()),
           QStringLiteral("#FFD54F"));
    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
           QStringLiteral("BUILD logRoot=%1 (file=%2)").arg(resolveLogRoot(),
               m_logFile ? m_logFile->fileName() : QStringLiteral("(disabled)")),
           QStringLiteral("#FFD54F"));
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

    panoramaView->setFixedHeight(80);
    thermalPanoramaView->setFixedHeight(100);
    panoramaView->setShowRuler(false);
    thermalPanoramaView->setShowRuler(true);

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

    layout->setRowStretch(0, 0);
    layout->setRowStretch(1, 4);
    layout->setRowStretch(2, 2);

    connect(colorRoiView, &AIVideoWidget::clickedAt, this, [=](QPoint) {
        addLog(QStringLiteral("ROI"), QStringLiteral("RGB ROI click (hasImg=%1 size=%2x%3)")
                   .arg(!m_lastColorRoi.isNull())
                   .arg(m_lastColorRoi.width())
                   .arg(m_lastColorRoi.height()),
               "#00AAAA");
        if (!m_panoCache || m_lastRoiAngle < 0.0) {
            addLog(QStringLiteral("ROI"), QStringLiteral("RGB ROI: no data"), "#F44336");
            if (ui->statusbar) ui->statusbar->showMessage(QStringLiteral("RGB ROI: no data"), 3000);
            return;
        }
        const double angle = m_lastRoiAngle;
        RoiPopupDialog *dlg = new RoiPopupDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        dlg->setWindowFlags(dlg->windowFlags() | Qt::Window);
        dlg->setWindowTitle(QStringLiteral("RGB ROI  (+/- zoom, 0 reset)"));
        QPointer<RoiPopupDialog> p = dlg;
        dlg->setScaleCallback([=](double s) {
            if (!p || !m_roiWorker) return;
            QMetaObject::invokeMethod(m_roiWorker, "requestFullRgbScaled", Qt::QueuedConnection, Q_ARG(double, angle), Q_ARG(double, s));
        });
        connect(m_roiWorker, &RoiWorker::fullScaledReady, dlg, [=](bool isBw, double a, double actualScale, const QImage &img) {
            if (!p || isBw) return;
            if (qAbs(a - angle) > 0.001) return;
            p->m_scale = actualScale;
            p->setImage(img);
        }, Qt::QueuedConnection);
        if (QApplication::desktop()) {
            const QRect g = QApplication::desktop()->availableGeometry(this);
            dlg->move(g.center() - QPoint(dlg->width() / 2, dlg->height() / 2));
        }
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
        if (ui->statusbar) ui->statusbar->showMessage(QStringLiteral("Extracting RGB ROI..."), 1200);
        if (m_roiWorker) QMetaObject::invokeMethod(m_roiWorker, "requestFullRgbScaled", Qt::QueuedConnection, Q_ARG(double, angle), Q_ARG(double, 1.0));
    });
    connect(thermalRoiView, &AIVideoWidget::clickedAt, this, [=](QPoint) {
        addLog(QStringLiteral("ROI"), QStringLiteral("BW ROI click (hasImg=%1 size=%2x%3)")
                   .arg(!m_lastThermalRoi.isNull())
                   .arg(m_lastThermalRoi.width())
                   .arg(m_lastThermalRoi.height()),
               "#00AAAA");
        if (!m_panoCache || m_lastRoiAngle < 0.0) {
            addLog(QStringLiteral("ROI"), QStringLiteral("BW ROI: no data"), "#F44336");
            if (ui->statusbar) ui->statusbar->showMessage(QStringLiteral("BW ROI: no data"), 3000);
            return;
        }
        const double angle = m_lastRoiAngle;
        RoiPopupDialog *dlg = new RoiPopupDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        dlg->setWindowFlags(dlg->windowFlags() | Qt::Window);
        dlg->setWindowTitle(QStringLiteral("BW ROI  (+/- zoom, 0 reset)"));
        QPointer<RoiPopupDialog> p = dlg;
        dlg->setScaleCallback([=](double s) {
            if (!p || !m_roiWorker) return;
            QMetaObject::invokeMethod(m_roiWorker, "requestFullBwScaled", Qt::QueuedConnection, Q_ARG(double, angle), Q_ARG(double, s));
        });
        connect(m_roiWorker, &RoiWorker::fullScaledReady, dlg, [=](bool isBw, double a, double actualScale, const QImage &img) {
            if (!p || !isBw) return;
            if (qAbs(a - angle) > 0.001) return;
            p->m_scale = actualScale;
            p->setImage(img);
        }, Qt::QueuedConnection);
        if (QApplication::desktop()) {
            const QRect g = QApplication::desktop()->availableGeometry(this);
            dlg->move(g.center() - QPoint(dlg->width() / 2, dlg->height() / 2));
        }
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
        if (ui->statusbar) ui->statusbar->showMessage(QStringLiteral("Extracting BW ROI..."), 1200);
        if (m_roiWorker) QMetaObject::invokeMethod(m_roiWorker, "requestFullBwScaled", Qt::QueuedConnection, Q_ARG(double, angle), Q_ARG(double, 1.0));
    });

    {
        QImage blackRgb(8192, 240, QImage::Format_RGB32);
        blackRgb.fill(Qt::black);
        panoramaView->updateImage(blackRgb);
        thermalPanoramaView->updateImage(blackRgb);
    }

    // ~30fps steady full-redraw of the panoramas (see onPanoRefreshTick).
    m_panoRefreshTimer = new QTimer(this);
    m_panoRefreshTimer->setInterval(33);
    connect(m_panoRefreshTimer, &QTimer::timeout, this, &MainWindow::onPanoRefreshTick);
    m_panoRefreshTimer->start();

    m_latestAngle = 0.0;
    m_prevCheckAngle = 0.0;

    m_driver = new TurntableDriver(this);
    m_ctrlDialog = new TurntableControlDialog(m_driver, this);

    QShortcut *shortcutF5 = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(shortcutF5, &QShortcut::activated, this, &MainWindow::onClearUiClicked);
    QShortcut *shortcutCtrlR = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(shortcutCtrlR, &QShortcut::activated, this, &MainWindow::onClearUiClicked);

    connect(m_driver, &TurntableDriver::angleUpdated, this, [=](double realAngle){
        if (!m_zeroAngleInited) {
            m_zeroAngleInited = true;
            m_zeroAngleRaw = realAngle;
        }
        const double displayAngle = toRelativeAngle(realAngle);
        m_latestAngle = displayAngle;
        radarView->setCurrentAngle(displayAngle);
        m_angleLabel->setText(QString("%1°").arg(displayAngle, 0, 'f', 2));
        if (m_colorThread) m_colorThread->setCurrentAngle(displayAngle);
        if (m_thermalThread) m_thermalThread->setCurrentAngle(displayAngle);

        static bool speedInit = false;
        static double prevA = 0.0;
        static qint64 prevMs = 0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (!speedInit) {
            speedInit = true;
            prevA = displayAngle;
            prevMs = nowMs;
            return;
        }
        const qint64 dtMs = nowMs - prevMs;
        if (dtMs <= 0) return;
        double diff = qAbs(displayAngle - prevA);
        if (diff > 180.0) diff = 360.0 - diff;
        const double dtSec = (double)dtMs / 1000.0;
        const double degPerSec = (dtSec > 0.0001) ? (diff / dtSec) : 0.0;
        prevA = displayAngle;
        prevMs = nowMs;
    });

    connect(m_driver, &TurntableDriver::lapTimeMeasured, this, [=](double lapTime){
        m_lapTimeLabel->setText(QString("Lap: %1 s").arg(lapTime, 0, 'f', 2));
    });

    // ====================================================================
    m_cmdSocket = new QUdpSocket(this);
    m_replySocket = new QUdpSocket(this);

    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE6\x8E\xA7\xE5\x88\xB6\xE5\x87\x86\xE5\xA4\x87\xE5\xB0\xB1\xE7\xBB\xAA\xEF\xBC\x8C\xE7\x9B\xAE\xE6\xA0\x87\x3A\x20\x25\x31").arg(DEVICE_IP), "#569CD6");

    // bind 5002
    if (m_replySocket->bind(QHostAddress::AnyIPv4, CMD_PORT_REPLY, QUdpSocket::ShareAddress)) {
        qDebug() << ">>> [UDP] bind 5002 ok";
        addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), u8s("\xE6\x88\x90\xE5\x8A\x9F\xE7\xBB\x91\xE5\xAE\x9A\x20\x35\x30\x30\x32\x20\xE7\xAB\xAF\xE5\x8F\xA3\xEF\xBC\x8C\xE7\x9B\x91\xE5\x90\xAC\xE8\xAE\xBE\xE5\xA4\x87\xE5\xBA\x94\xE7\xAD\x94"), "#6A9955");
    } else {
        qDebug() << ">>> [UDP] bind 5002 failed";
        addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), u8s("\xE9\x94\x99\xE8\xAF\xAF\xEF\xBC\x9A\xE6\x97\xA0\xE6\xB3\x95\xE7\xBB\x91\xE5\xAE\x9A\x20\x35\x30\x30\x32\x20\xE7\xAB\xAF\xE5\x8F\xA3"), "#F44336");
    }

    connect(m_replySocket, &QUdpSocket::readyRead, this, &MainWindow::onCommandReplyReceived);

    m_colorThread = new VideoThread(0, m_panoCache, this);
    m_thermalThread = new VideoThread(1, m_panoCache, this);
    m_pathThread = new VideoThread(2, QSharedPointer<PanoramaCache>(), this);
    if (m_recordWorker) {
        m_colorThread->setRecorder(m_recordWorker);
        m_thermalThread->setRecorder(m_recordWorker);
    }

    connect(m_colorThread, &VideoThread::logRequested, this, &MainWindow::addLog);
    connect(m_thermalThread, &VideoThread::logRequested, this, &MainWindow::addLog);
    connect(m_pathThread, &VideoThread::logRequested, this, &MainWindow::addLog);

    connect(m_pathThread, &VideoThread::pathReceived, this, &MainWindow::onPathReceived, Qt::QueuedConnection);

    m_pathThread->start();
    m_colorThread->start();
    m_thermalThread->start();

    connect(m_colorThread, &VideoThread::cacheUpdated, this, &MainWindow::onRenderTick, Qt::QueuedConnection);
    connect(m_thermalThread, &VideoThread::cacheUpdated, this, &MainWindow::onRenderTick, Qt::QueuedConnection);

    connect(panoramaView, SIGNAL(angleSelected(double)), this, SLOT(onPanoramaClicked(double)));
    connect(thermalPanoramaView, SIGNAL(angleSelected(double)), this, SLOT(onPanoramaClicked(double)));
    connect(radarView, SIGNAL(sectorClicked(int)), this, SLOT(onRadarClicked(int)));

    initSimulatedTargets();
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    QMainWindow::mousePressEvent(event);
}

MainWindow::~MainWindow()
{
    if(m_driver) { m_driver->stop(); m_driver->closePort(); }
    if(m_pathThread) { m_pathThread->stop(); m_pathThread->wait(); }
    if(m_colorThread) { m_colorThread->stop(); m_colorThread->wait(); }
    if(m_thermalThread) { m_thermalThread->stop(); m_thermalThread->wait(); }
    if (m_saveThread) { m_saveThread->quit(); m_saveThread->wait(); }
    if (m_roiThread) { m_roiThread->quit(); m_roiThread->wait(); }
    if (m_recordThread) { m_recordThread->quit(); m_recordThread->wait(); }
    if (m_logFile) {
        if (m_logFile->isOpen()) m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
    delete ui;
}

void MainWindow::setupLogDock()
{
    QDockWidget *dock = new QDockWidget(u8s("\xE7\xB3\xBB\xE7\xBB\x9F\xE9\x80\x9A\xE4\xBF\xA1\xE6\x97\xA5\xE5\xBF\x97"), this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_logBrowser = new QTextBrowser(dock);
    m_logBrowser->setStyleSheet("QTextBrowser { background-color: #16161e; color: #d4d4d4; font-family: 'Consolas','Source Code Pro','DejaVu Sans Mono','Microsoft YaHei UI',monospace; font-size: 10pt; border: 1px solid #3a3a4e; border-radius: 4px; }");
    {
        QFont f(QStringLiteral("Consolas"));
        f.setStyleHint(QFont::Monospace);
        f.setPointSize(10);
        m_logBrowser->setFont(f);
        m_logBrowser->document()->setDefaultFont(f);
    }
    m_logBrowser->document()->setMaximumBlockCount(1200);
    dock->setWidget(m_logBrowser);

    addDockWidget(Qt::LeftDockWidgetArea, dock);
    m_mainToolBar->addAction(dock->toggleViewAction());
}

void MainWindow::addLog(const QString &type, const QString &msg, const QString &color)
{
    const QTime ct = QTime::currentTime();
    char tsBuf[32];
    qsnprintf(tsBuf, sizeof(tsBuf), "%02d:%02d:%02d.%03d",
        ct.hour(), ct.minute(), ct.second(), ct.msec());

    if (m_logFile && m_logFile->isOpen()) {
        const QByteArray line = (QStringLiteral("[") + QString::fromLatin1(tsBuf) + QStringLiteral("] [")
            + type + QStringLiteral("] ") + msg + QStringLiteral("\n")).toUtf8();
        m_logFile->write(line);
        m_logFile->flush();
    }

    if (!m_logBrowser) return;
    const qint64 nowMs = m_perfTimer.isValid() ? m_perfTimer.elapsed() : 0;
    const bool important =
        type.contains(u8s("\xE9\x94\x99\xE8\xAF\xAF")) ||
        type.contains(u8s("\xE8\xAF\xBB\xE5\x8F\x96\xE5\xA4\xB1\xE8\xB4\xA5")) ||
        type.contains(u8s("\xE7\xB3\xBB\xE7\xBB\x9F")) ||
        type.contains(QStringLiteral("ROI")) ||
        type.startsWith(QStringLiteral("RX("));
    if (!important && (nowMs - m_lastLogMs) < 15) return;
    m_lastLogMs = nowMs;
    const QString timeStr = QString::fromLatin1(tsBuf);

    QTextCursor c = m_logBrowser->textCursor();
    c.movePosition(QTextCursor::End);

    QTextCharFormat fmtTime;
    fmtTime.setForeground(QColor(QStringLiteral("gray")));

    QTextCharFormat fmtType;
    fmtType.setForeground(QColor(color));
    fmtType.setFontWeight(QFont::Bold);

    QTextCharFormat fmtMsg;
    fmtMsg.setForeground(QColor(QStringLiteral("#D4D4D4")));

    c.insertText(QStringLiteral("[%1] ").arg(timeStr), fmtTime);
    c.insertText(QStringLiteral("[%1] ").arg(type), fmtType);
    c.insertText(msg, fmtMsg);
    c.insertText(QStringLiteral("\n"));

    m_logBrowser->setTextCursor(c);
    m_logBrowser->ensureCursorVisible();
}

void MainWindow::sendCommand(const QString &cmd)
{
    QByteArray data = cmd.toUtf8();
    m_cmdSocket->writeDatagram(data, QHostAddress(DEVICE_IP), CMD_PORT_SEND);
    qDebug() << ">>> [UDP TX] ->" << DEVICE_IP << ":" << CMD_PORT_SEND << "|" << cmd;
    addLog(u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE4\xB8\x8B\xE5\x8F\x91\x20\x28\x35\x30\x30\x31\x29"), cmd, "#569CD6");
}

void MainWindow::onCommandReplyReceived()
{
    while (m_replySocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_replySocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;

        m_replySocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        QString replyStr = QString::fromUtf8(datagram);
        qDebug() << "<<< [UDP RX] <-" << sender.toString() << ":" << senderPort << "|" << replyStr;
        addLog(u8s("\xE8\xAE\xBE\xE5\xA4\x87\xE5\xBA\x94\xE7\xAD\x94\x20\x28\x35\x30\x30\x32\x29"), replyStr, "#6A9955");

        if(ui->statusbar) {
            ui->statusbar->showMessage(u8s("\xE7\xA1\xAC\xE4\xBB\xB6\xE5\x9B\x9E\xE4\xBC\xA0\x3A\x20\x25\x31").arg(replyStr), 5000);
        }
    }
}

void MainWindow::createToolBar()
{
    m_mainToolBar = new QToolBar("Toolbar", this);
    addToolBar(Qt::TopToolBarArea, m_mainToolBar);
    m_mainToolBar->setMovable(false);

    m_mainToolBar->setFixedHeight(40);
    m_mainToolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    m_actOpenDevice = new QAction(u8s("\xE8\xAE\xBE\xE5\xA4\x87\xE8\xBF\x90\xE8\xA1\x8C"), this);
    m_actCloseDevice = new QAction(u8s("\xE8\xAE\xBE\xE5\xA4\x87\xE5\x81\x9C\xE6\xAD\xA2"), this);
    m_actSavePng = new QAction(u8s("\xE6\x97\xA0\xE6\x8D\x9F\xE9\x87\x87\xE9\x9B\x86"), this);
    m_actSaveJpg = new QAction(u8s("\xE5\x8E\x8B\xE7\xBC\xA9\xE9\x87\x87\xE9\x9B\x86"), this);
    m_actSaveVideo = new QAction(u8s("\xE5\xAE\x9E\xE6\x97\xB6\xE7\xBD\x91\xE7\xBB\x9C\xE9\x87\x87\xE9\x9B\x86"), this);
    m_actStopCapture = new QAction(u8s("\xE5\x81\x9C\xE6\xAD\xA2\xE9\x87\x87\xE9\x9B\x86"), this);
    m_actClearImage = new QAction(u8s("\xE6\xB8\x85\xE9\x99\xA4\xE5\x9B\xBE\xE5\x83\x8F"), this);
    m_actSaveFullPanorama = new QAction(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), this);
    m_actRecord = new QAction(u8s("\xE5\xBC\x80\xE5\xA7\x8B\xE5\xBD\x95\xE5\x88\xB6"), this);
    m_actRecord->setCheckable(true);
    m_actExit = new QAction(u8s("\xE9\x80\x80\xE5\x87\xBA\xE7\xB3\xBB\xE7\xBB\x9F"), this);

    m_mainToolBar->addAction(m_actOpenDevice);
    m_mainToolBar->addAction(m_actCloseDevice);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actSavePng);
    m_mainToolBar->addAction(m_actSaveJpg);
    m_mainToolBar->addAction(m_actSaveVideo);
    m_mainToolBar->addAction(m_actStopCapture);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actClearImage);
    m_mainToolBar->addAction(m_actRecord);

    QAction *actOpenTurntable = m_mainToolBar->addAction(u8s("\xE8\xBD\xAC\xE5\x8F\xB0\xE6\x8E\xA7\xE5\x88\xB6"));
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
    connect(m_actRecord, &QAction::triggered, this, &MainWindow::onToggleRecording);
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
    if (m_actRecord) m_actRecord->setEnabled(m_isDeviceOpen);

    if (m_actSaveFullPanorama) {
        bool canSave = false;
        QString tip = u8s("\xE5\x85\xA8\xE6\x99\xAF\xE7\xBC\x93\xE5\xAD\x98\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA");
        if (m_panoCache) {
            const PanoramaCache::BlockState rgbFull = m_panoCache->state(PanoramaCache::FullRgb);
            const PanoramaCache::BlockState bwFull = m_panoCache->state(PanoramaCache::FullBw);
            const PanoramaCache::BlockState rgbThumb = m_panoCache->state(PanoramaCache::ThumbRgb);
            const PanoramaCache::BlockState bwThumb = m_panoCache->state(PanoramaCache::ThumbBw);
            if (!rgbFull.inited) {
                tip = (rgbThumb.inited && rgbThumb.validFrames > 0)
                    ? u8s("\xE5\xBD\xA9\xE8\x89\xB2\xE6\x97\xA0\xE6\x8D\x9F\xE5\x85\xA8\xE6\x99\xAF\xE5\x86\x85\xE5\xAD\x98\xE4\xB8\x8D\xE5\x8F\xAF\xE7\x94\xA8\xEF\xBC\x88\xE7\xBC\xA9\xE7\x95\xA5\xE5\xB7\xB2\xE6\x94\xB6\xE5\x88\xB0\xE5\xB8\xA7\xEF\xBC\x89")
                    : u8s("\xE5\xBD\xA9\xE8\x89\xB2\xE6\x97\xA0\xE6\x8D\x9F\xE5\x85\xA8\xE6\x99\xAF\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA\xEF\xBC\x88\xE5\xB0\x9A\xE6\x9C\xAA\xE6\x94\xB6\xE5\x88\xB0\xE5\xB8\xA7\xE6\x88\x96\xE8\xA7\xA3\xE7\xA0\x81\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x89");
            } else if (!bwFull.inited) {
                tip = (bwThumb.inited && bwThumb.validFrames > 0)
                    ? u8s("\xE9\xBB\x91\xE7\x99\xBD\xE6\x97\xA0\xE6\x8D\x9F\xE5\x85\xA8\xE6\x99\xAF\xE5\x86\x85\xE5\xAD\x98\xE4\xB8\x8D\xE5\x8F\xAF\xE7\x94\xA8\xEF\xBC\x88\xE7\xBC\xA9\xE7\x95\xA5\xE5\xB7\xB2\xE6\x94\xB6\xE5\x88\xB0\xE5\xB8\xA7\xEF\xBC\x89")
                    : u8s("\xE9\xBB\x91\xE7\x99\xBD\xE6\x97\xA0\xE6\x8D\x9F\xE5\x85\xA8\xE6\x99\xAF\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA\xEF\xBC\x88\xE5\xB0\x9A\xE6\x9C\xAA\xE6\x94\xB6\xE5\x88\xB0\xE5\xB8\xA7\xE6\x88\x96\xE8\xA7\xA3\xE7\xA0\x81\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x89");
            } else if (rgbFull.segments <= 0 || bwFull.segments <= 0) {
                tip = u8s("\xE5\x85\xA8\xE6\x99\xAF\xE5\x88\x86\xE6\xAE\xB5\xE6\x9C\xAA\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96");
            } else if (rgbFull.validFrames <= 0 || bwFull.validFrames <= 0) {
                tip = u8s("\xE5\x85\xA8\xE6\x99\xAF\xE5\xB0\x9A\xE6\x97\xA0\xE6\x9C\x89\xE6\x95\x88\xE5\xB8\xA7");
            } else {
                const bool complete = (rgbFull.validFrames == rgbFull.segments && bwFull.validFrames == bwFull.segments);
                canSave = true;
                tip = complete
                    ? u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\xBD\x93\xE5\x89\x8D\xE5\x85\xA8\xE6\x99\xAF")
                    : u8s("\xE6\x9C\xAA\xE6\xBB\xA1\xE5\x9C\x88\xEF\xBC\x9A\x52\x47\x42\x20\x25\x31\x2F\x25\x32\x20\x42\x57\x20\x25\x33\x2F\x25\x34")
                          .arg(rgbFull.validFrames).arg(rgbFull.segments).arg(bwFull.validFrames).arg(bwFull.segments);
            }
        }
        m_actSaveFullPanorama->setToolTip(tip);
        m_actSaveFullPanorama->setEnabled(canSave);
    }
}

void MainWindow::onToggleRecording()
{
    if (!m_actRecord) return;
    if (!m_recordWorker) {
        m_actRecord->setChecked(false);
        return;
    }
    if (!m_isDeviceOpen) {
        m_actRecord->setChecked(false);
        return;
    }

    const bool enable = m_actRecord->isChecked();
    m_isRecording = enable;

    if (m_colorThread) m_colorThread->setRecordingEnabled(enable);
    if (m_thermalThread) m_thermalThread->setRecordingEnabled(enable);

    if (enable) {
        QMetaObject::invokeMethod(m_recordWorker, "startRecording", Qt::QueuedConnection, Q_ARG(QString, resolveRecordRoot()), Q_ARG(int, 10));
        addLog(QStringLiteral("REC"), QStringLiteral("Start"), QStringLiteral("#569CD6"));
        return;
    }
    QMetaObject::invokeMethod(m_recordWorker, "stopRecording", Qt::QueuedConnection);
    addLog(QStringLiteral("REC"), QStringLiteral("Stop"), QStringLiteral("#569CD6"));
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

void MainWindow::onActionOpenDevice()
{
    sendCommand("TG_OPEN_DEVICE;");
    m_isDeviceOpen = true;
    updateUiState();
    ui->statusbar->showMessage(u8s("\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x90\x91\xE8\xAE\xBE\xE5\xA4\x87\xE4\xB8\x8B\xE5\x8F\x91\xE4\xBD\xBF\xE8\x83\xBD\xE5\x91\xBD\xE4\xBB\xA4\x2E\x2E\x2E"), 2000);
}

void MainWindow::onActionCloseDevice()
{
    sendCommand("TG_CLOSE_DEVICE;");
    if (m_driver) m_driver->stop();
    m_isDeviceOpen = false;
    updateUiState();
    ui->statusbar->showMessage(u8s("\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x90\x91\xE8\xAE\xBE\xE5\xA4\x87\xE4\xB8\x8B\xE5\x8F\x91\xE5\x81\x9C\xE6\xAD\xA2\xE5\x91\xBD\xE4\xBB\xA4\x2E\x2E\x2E"), 2000);
}

void MainWindow::onActionSavePng()
{
    sendCommand("TG_SAVE_PNG;");
    ui->statusbar->showMessage(u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE5\xB7\xB2\xE4\xB8\x8B\xE5\x8F\x91\x3A\x20\xE5\x90\xAF\xE5\x8A\xA8\xE6\x97\xA0\xE6\x8D\x9F\xE9\x87\x87\xE9\x9B\x86\x2E\x2E\x2E"), 2000);
}

void MainWindow::onActionSaveJpg()
{
    sendCommand("TG_SAVE_JPG;");
    ui->statusbar->showMessage(u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE5\xB7\xB2\xE4\xB8\x8B\xE5\x8F\x91\x3A\x20\xE5\x90\xAF\xE5\x8A\xA8\xE5\x8E\x8B\xE7\xBC\xA9\xE9\x87\x87\xE9\x9B\x86\x2E\x2E\x2E"), 2000);
}

void MainWindow::onActionSaveVideo()
{
    sendCommand("TG_SAVE_VIDEO;");
    ui->statusbar->showMessage(u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE5\xB7\xB2\xE4\xB8\x8B\xE5\x8F\x91\x3A\x20\xE5\x90\xAF\xE5\x8A\xA8\xE5\xAE\x9E\xE6\x97\xB6\xE7\xBD\x91\xE7\xBB\x9C\xE8\xA7\x86\xE9\xA2\x91\xE9\x87\x87\xE9\x9B\x86\x2E\x2E\x2E"), 2000);
}

void MainWindow::onActionStopCapture()
{
    sendCommand("TG_STOP_CAPTURE;");
    ui->statusbar->showMessage(u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE5\xB7\xB2\xE4\xB8\x8B\xE5\x8F\x91\x3A\x20\xE5\x81\x9C\xE6\xAD\xA2\xE5\x9B\xBE\xE5\x83\x8F\xE9\x87\x87\xE9\x9B\x86\xEF\xBC\x81"), 2000);
}

void MainWindow::onSaveFullPanoramaClicked()
{
    if (!m_panoCache) {
        if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE5\x85\xA8\xE6\x99\xAF\xE7\xBC\x93\xE5\xAD\x98\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), 3000);
        addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), u8s("\xE6\x8B\xA6\xE6\x88\xAA\xEF\xBC\x9A\xE5\x85\xA8\xE6\x99\xAF\xE7\xBC\x93\xE5\xAD\x98\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA"), "#F44336");
        return;
    }

    const PanoramaCache::BlockState rgb = m_panoCache->state(PanoramaCache::FullRgb);
    const PanoramaCache::BlockState bw = m_panoCache->state(PanoramaCache::FullBw);
    if (!rgb.inited || !bw.inited || rgb.segments <= 0 || bw.segments <= 0 || rgb.validFrames <= 0 || bw.validFrames <= 0) {
        if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE5\x85\xA8\xE6\x99\xAF\xE7\xBC\x93\xE5\xAD\x98\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA\xEF\xBC\x88\x52\x47\x42\x2F\x42\x57\xE6\x97\xA0\xE6\x8D\x9F\xE5\x85\xA8\xE6\x99\xAF\xE4\xB8\x8D\xE5\x8F\xAF\xE7\x94\xA8\xEF\xBC\x89\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), 4000);
        addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), u8s("\xE6\x8B\xA6\xE6\x88\xAA\xEF\xBC\x9A\xE7\xBC\x93\xE5\xAD\x98\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA\x20\x72\x67\x62\x2E\x69\x6E\x69\x74\x65\x64\x3D\x25\x31\x20\x62\x77\x2E\x69\x6E\x69\x74\x65\x64\x3D\x25\x32\x20\x72\x67\x62\x2E\x76\x61\x6C\x69\x64\x3D\x25\x33\x2F\x25\x34\x20\x62\x77\x2E\x76\x61\x6C\x69\x64\x3D\x25\x35\x2F\x25\x36")
               .arg(rgb.inited).arg(bw.inited)
               .arg(rgb.validFrames).arg(rgb.segments)
               .arg(bw.validFrames).arg(bw.segments), "#F44336");
        return;
    }

    const bool complete = (rgb.validFrames == rgb.segments && bw.validFrames == bw.segments);
    if (!complete) {
        if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE5\x85\xA8\xE6\x99\xAF\xE6\x9C\xAA\xE6\xBB\xA1\xE5\x9C\x88\xEF\xBC\x9A\xE4\xBF\x9D\xE5\xAD\x98\xE4\xBC\x9A\xE5\x8C\x85\xE5\x90\xAB\xE7\xA9\xBA\xE7\x99\xBD\xE5\x8C\xBA\xE5\x9F\x9F"), 3000);
        addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), QString("Incomplete: RGB=%1/%2 BW=%3/%4").arg(rgb.validFrames).arg(rgb.segments).arg(bw.validFrames).arg(bw.segments), "#FFA500");
    }

    const QString baseDir = resolveSaveRoot();
    if (!QDir().mkpath(baseDir)) {
        addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), u8s("\xE5\x88\x9B\xE5\xBB\xBA\xE7\x9B\xAE\xE5\xBD\x95\xE5\xA4\xB1\xE8\xB4\xA5\x3A\x20") + baseDir, "#F44336");
        return;
    }
    const QString ts = makeAsciiTimestamp(QDateTime::currentDateTime(), QStringLiteral("SAVE2_"));
    const QString outDir = QDir(baseDir).filePath(ts);
    if (!QDir().mkpath(outDir)) {
        addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), u8s("\xE5\x88\x9B\xE5\xBB\xBA\xE7\x9B\xAE\xE5\xBD\x95\xE5\xA4\xB1\xE8\xB4\xA5\x3A\x20") + outDir, "#F44336");
        return;
    }
    const int rgbQuality = 95;
    addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), u8s("\xE5\xB7\xB2\xE5\x85\xA5\xE9\x98\x9F\x3A\x20") + outDir, "#569CD6");
    if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE5\xB7\xB2\xE5\x85\xA5\xE9\x98\x9F\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE6\x99\xAF\x2E\x2E\x2E"), 2000);
    emit savePanoramaRequested(outDir, rgbQuality);
}

void MainWindow::onSaveFullPanoramaFinished(quint64 saveId, bool ok, const QString &msg, const QString &outDir)
{
    if (ok) {
        addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), QString("%1 OK: %2").arg(saveId).arg(outDir), "#6A9955");
        if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE\xE6\x88\x90\xE5\x8A\x9F"), 2500);
        return;
    }
    addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), QString("%1 FAIL: %2 (%3)").arg(saveId).arg(msg).arg(outDir), "#F44336");
    if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE\xE5\xA4\xB1\xE8\xB4\xA5\x3A\x20") + msg, 4000);
}

void MainWindow::onClearUiClicked()
{
    {
        QImage rgb(8192, 240, QImage::Format_RGB32);
        rgb.fill(Qt::black);
        QImage bw(8192, 240, QImage::Format_Indexed8);
        QVector<QRgb> table;
        table.reserve(256);
        for (int i = 0; i < 256; ++i) table.push_back(qRgb(i, i, i));
        bw.setColorTable(table);
        bw.fill(0);
        m_uiThumbRgb = rgb;
        m_uiThumbBw = bw;
        panoramaView->updateImage(m_uiThumbRgb);
        thermalPanoramaView->updateImage(m_uiThumbBw);
    }

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
    if(m_logBrowser) m_logBrowser->clear();

    for(auto& target : m_simTargets) { target.isDetected = false; }
    radarView->setTargets(m_simTargets);

    if (m_panoCache) m_panoCache->resetAll();
    updateUiState();

    if(ui->statusbar) ui->statusbar->showMessage(u8s("\xE7\x95\x8C\xE9\x9D\xA2\xE7\xBC\x93\xE5\xAD\x98\xE5\xB7\xB2\xE6\xB8\x85\xE7\xA9\xBA\xEF\xBC\x81"), 2000);
}

void MainWindow::onPanoramaClicked(double angle)
{
    panoramaView->setSelectedAngle(angle);
    thermalPanoramaView->setSelectedAngle(angle);

    if (!m_panoCache) return;
    m_lastRoiAngle = angle;
    if (m_roiWorker) {
        QMetaObject::invokeMethod(m_roiWorker, "requestPreview", Qt::QueuedConnection, Q_ARG(double, angle));
    }
}

void MainWindow::onThumbRoiReady(double angle, const QImage &rgb, const QImage &bwRgb32)
{
    if (m_lastRoiAngle >= 0.0 && qAbs(m_lastRoiAngle - angle) > 0.001) return;
    if (!rgb.isNull()) {
        m_lastColorRoi = rgb;
        if (colorRoiView) colorRoiView->updateImage(rgb);
    }
    if (!bwRgb32.isNull()) {
        m_lastThermalRoi = bwRgb32;
        if (thermalRoiView) thermalRoiView->updateImage(bwRgb32);
    }
}

void MainWindow::onRenderTick()
{
    if (m_renderPending.testAndSetAcquire(0, 1)) {
        QMetaObject::invokeMethod(this, "drainRender", Qt::QueuedConnection);
    }
}

void MainWindow::drainRender()
{
    m_renderPending.storeRelease(0);
    if (!m_panoCache) return;

    QRect dirtyRgb;
    QRect dirtyBw;
    const PanoramaCache::ThumbInfo infoRgb = m_panoCache->thumbInfoRgb();
    const PanoramaCache::ThumbInfo infoBw = m_panoCache->thumbInfoBw();
    if (infoRgb.inited && !m_uiThumbRgb.isNull() && m_uiThumbRgb.format() == infoRgb.format &&
        m_uiThumbRgb.width() == infoRgb.panoW && m_uiThumbRgb.height() == infoRgb.panoH) {
        const QVector<int> tiles = m_panoCache->takeDirtyThumbRgbTiles();
        for (int t : tiles) {
            if (!m_panoCache->blitThumbRgbTileTo(t, m_uiThumbRgb)) continue;
            const int x = t * infoRgb.sliceW;
            dirtyRgb |= QRect(x, 0, infoRgb.sliceW, infoRgb.panoH);
        }
    }
    if (infoBw.inited && !m_uiThumbBw.isNull() && m_uiThumbBw.format() == infoBw.format &&
        m_uiThumbBw.width() == infoBw.panoW && m_uiThumbBw.height() == infoBw.panoH) {
        const QVector<int> tiles = m_panoCache->takeDirtyThumbBwTiles();
        for (int t : tiles) {
            if (!m_panoCache->blitThumbBwTileTo(t, m_uiThumbBw)) continue;
            const int x = t * infoBw.sliceW;
            dirtyBw |= QRect(x, 0, infoBw.sliceW, infoBw.panoH);
        }
    }
    // Keep m_uiThumb* up to date (cheap incremental blit above), but defer the
    // actual widget repaint to the steady 30fps timer (onPanoRefreshTick) which
    // redraws the WHOLE panorama at once -> smooth, no tile-by-tile blockiness.
    if (!dirtyRgb.isNull()) m_panoRgbDirty = true;
    if (!dirtyBw.isNull()) m_panoBwDirty = true;

    const qint64 nowMs = m_perfTimer.isValid() ? m_perfTimer.elapsed() : 0;
    if ((nowMs - m_lastDetectMs) >= 120) {
        m_lastDetectMs = nowMs;
        checkTargetDetection(m_latestAngle);
    }
    if (m_prevCheckAngle > 300.0 && m_latestAngle < 60.0) {
        bool needReset = false;
        for (auto &target : m_simTargets) {
            if (target.isDetected) { target.isDetected = false; needReset = true; }
        }
        if (needReset) radarView->setTargets(m_simTargets);
    }
    m_prevCheckAngle = m_latestAngle;
    updateUiState();
}

void MainWindow::onPanoRefreshTick()
{
    // Steady ~30fps full-image redraw of both panoramas, decoupled from data
    // arrival. Only repaint when something actually changed since last tick.
    if (m_panoRgbDirty && panoramaView && !m_uiThumbRgb.isNull()) {
        panoramaView->updateImage(m_uiThumbRgb);
        m_panoRgbDirty = false;
    }
    if (m_panoBwDirty && thermalPanoramaView && !m_uiThumbBw.isNull()) {
        thermalPanoramaView->updateImage(m_uiThumbBw);
        m_panoBwDirty = false;
    }
}

void MainWindow::onPathReceived(const QString &type, const QString &path, const QString &sender, qint64 rxMs)
{
    const QString t = type.trimmed().toUpper();
    const double angleDeg = m_zeroAngleInited ? m_latestAngle : -1.0;
    if (t == "RGB") {
        if (m_colorThread) m_colorThread->enqueuePath(t, path, sender, angleDeg, rxMs);
        return;
    }
    if (t == "BW" || t == "GRAY") {
        if (m_thermalThread) m_thermalThread->enqueuePath(t, path, sender, angleDeg, rxMs);
        return;
    }
}

void MainWindow::onRadarClicked(int angle)
{
    if (!m_panoCache) return;
    QImage roi = m_panoCache->extractFullRgbSliceByAngle(angle);
    if (roi.isNull()) return;
    radarFeedbackView->updateImage(roi);
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
                if (m_panoCache) {
                    QImage roi = m_panoCache->extractFullRgbSliceByAngle(target.angle);
                    if (!roi.isNull()) {
                        QImage targetImg = roi;
                        QPainter p(&targetImg);
                        p.setPen(QPen(Qt::red, 8));
                        p.drawRect(targetImg.rect().adjusted(8, 8, -8, -8));
                        p.setPen(Qt::green);
                        p.setFont(QFont("Arial", 40, QFont::Bold));
                        p.drawText(40, 80, QString("DETECTED: %1 deg").arg(m_pendingCaptureAngle));
                        p.end();
                        captureView->updateImage(targetImg);
                    }
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
