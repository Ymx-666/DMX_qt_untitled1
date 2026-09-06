#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDebug>
#include <QtMath>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QImageWriter>
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
#include "appconfig.h"
#include "radar_ui/compacttargetradarpanel.h"
#include "radar_ui/targetradarwindow.h"
#ifdef DMX_ADVANCED_DETECTION
#include "directyolomanager.h"
#endif

static inline QString u8s(const char *s) { return QString::fromUtf8(s); }

static QString resolveSaveRoot()
{
    return AppConfig::instance().saveRoot;
}

static QString resolveRecordRoot()
{
    return AppConfig::instance().recordRoot;
}

static QString resolveLogRoot()
{
    return AppConfig::instance().logRoot;
}

static bool parseReplayFileIndex(const QString &path, quint64 *out)
{
    const QString base = QFileInfo(path).completeBaseName();
    int pos = base.size() - 1;
    while (pos >= 0 && base.at(pos).isDigit()) --pos;
    const QString digits = base.mid(pos + 1);
    if (digits.isEmpty()) return false;
    bool ok = false;
    const quint64 value = digits.toULongLong(&ok);
    if (!ok) return false;
    if (out) *out = value;
    return true;
}

static double normalizeReplayAngle(double angleDeg)
{
    while (angleDeg < 0.0) angleDeg += 360.0;
    while (angleDeg >= 360.0) angleDeg -= 360.0;
    return angleDeg;
}

static double replayFrameAngle(quint64 fileIndex)
{
    const int segments = 16;
    const int sourceSlot = (int)(fileIndex % (quint64)segments);
    const int leftTurnTile = (segments - sourceSlot) % segments;
    return (double)leftTurnTile * (360.0 / (double)segments);
}


class RoiPopupDialog : public QDialog
{
public:
    explicit RoiPopupDialog(QWidget *parent = nullptr, bool centerViewportZoom = false)
        : QDialog(parent),
          m_centerViewportZoom(centerViewportZoom)
    {
        setWindowTitle("ROI");
        setSizeGripEnabled(true);
        resize(1000, 800);
        m_label = new QLabel();
        m_label->setBackgroundRole(QPalette::Base);
        m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_label->setScaledContents(true);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setCursor(Qt::OpenHandCursor);
        m_label->installEventFilter(this);

        m_scroll = new QScrollArea(this);
        m_scroll->setWidget(m_label);
        // Keep the label at the pixmap's natural size so the scroll area can
        // scroll a zoomed image (magnifier), instead of stretching to fit.
        m_scroll->setWidgetResizable(false);
        m_scroll->setAlignment(Qt::AlignCenter);
        m_scroll->viewport()->installEventFilter(this);

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_scroll);
        setLayout(layout);
    }

    void setImage(const QImage &img)
    {
        if (img.isNull() || !m_label) return;
        m_sourceImage = img.convertToFormat(QImage::Format_RGB32);
        updateDisplayedImage();

        // Restore the selected image point after the asynchronously scaled
        // image arrives.
        if (m_anchorFx >= 0.0 && m_scroll) {
            const double fx = m_anchorFx;
            const double fy = m_anchorFy;
            const QPoint viewportPos = m_anchorViewportPos;
            m_anchorFx = -1.0;
            m_anchorFy = -1.0;
            restoreZoomAnchor(fx, fy, viewportPos);
            QTimer::singleShot(0, this, [this, fx, fy, viewportPos]() {
                restoreZoomAnchor(fx, fy, viewportPos);
            });
        }
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!event) return;
        if (event->isAutoRepeat()) return;
        const int k = event->key();
        if ((event->modifiers() & Qt::ControlModifier) && k == Qt::Key_Z) {
            m_histStretch = !m_histStretch;
            updateDisplayedImage();
            return;
        }
        if (k == Qt::Key_F11 || k == Qt::Key_F) {
            toggleFullscreen();
            return;
        }
        if (k == Qt::Key_Escape && isFullScreen()) {
            showNormal();
            return;
        }
        if (k == Qt::Key_Plus || k == Qt::Key_Equal) {
            captureViewportCenterAnchor();
            m_scale *= 1.25;
            m_scale = qMin(5.0, m_scale);
            if (m_onScale) m_onScale(m_scale);
            return;
        }
        if (k == Qt::Key_Minus || k == Qt::Key_Underscore) {
            captureViewportCenterAnchor();
            m_scale /= 1.25;
            if (m_scale < 0.05) m_scale = 0.05;
            if (m_onScale) m_onScale(m_scale);
            return;
        }
        if (k == Qt::Key_0) {
            captureViewportCenterAnchor();
            m_scale = 1.0;
            if (m_onScale) m_onScale(m_scale);
            return;
        }
        QDialog::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (handleWheelZoom(event)) return;
        QDialog::wheelEvent(event);
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (!event || !m_scroll) return QDialog::eventFilter(obj, event);

        if (m_centerViewportZoom && event->type() == QEvent::Wheel) {
            QWheelEvent *wheel = static_cast<QWheelEvent*>(event);
            if (handleWheelZoom(wheel)) return true;
        }

        if (event->type() == QEvent::MouseButtonDblClick) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                toggleFullscreen();
                event->accept();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = true;
                m_lastDragPos = me->globalPos();
                if (m_label) m_label->setCursor(Qt::ClosedHandCursor);
                event->accept();
                return true;
            }
        }

        if (event->type() == QEvent::MouseMove && m_dragging) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            const QPoint delta = me->globalPos() - m_lastDragPos;
            m_lastDragPos = me->globalPos();
            if (m_scroll->horizontalScrollBar()) {
                m_scroll->horizontalScrollBar()->setValue(m_scroll->horizontalScrollBar()->value() - delta.x());
            }
            if (m_scroll->verticalScrollBar()) {
                m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->value() - delta.y());
            }
            event->accept();
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && m_dragging) {
                m_dragging = false;
                if (m_label) m_label->setCursor(Qt::OpenHandCursor);
                event->accept();
                return true;
            }
        }

        return QDialog::eventFilter(obj, event);
    }

private:
    friend class MainWindow;
    void setScaleCallback(std::function<void(double)> cb) { m_onScale = std::move(cb); }

    bool handleWheelZoom(QWheelEvent *event)
    {
        if (!event || !m_scroll || !m_label) return false;
        if (m_centerViewportZoom && !(event->modifiers() & Qt::ControlModifier)) return false;
        const int dy = event->angleDelta().y();
        if (dy == 0) return false;

        if (m_centerViewportZoom) {
            captureViewportCenterAnchor();
        } else {
            const QPoint vpPos = m_scroll->viewport()->mapFromGlobal(event->globalPos());
            captureZoomAnchor(vpPos);
        }

        m_scale *= (dy > 0) ? 1.25 : (1.0 / 1.25);
        if (m_scale < 0.05) m_scale = 0.05;
        m_scale = qMin(5.0, m_scale);
        if (m_onScale) m_onScale(m_scale);
        event->accept();
        return true;
    }

    void captureViewportCenterAnchor()
    {
        if (!m_centerViewportZoom || !m_scroll) return;
        captureZoomAnchor(m_scroll->viewport()->rect().center());
    }

    void captureZoomAnchor(const QPoint &viewportPos)
    {
        if (!m_scroll || !m_label || m_label->width() <= 0 || m_label->height() <= 0) return;
        const QPoint labelPos = m_label->mapFrom(m_scroll->viewport(), viewportPos);
        m_anchorFx = qBound(0.0, (double)labelPos.x() / (double)m_label->width(), 1.0);
        m_anchorFy = qBound(0.0, (double)labelPos.y() / (double)m_label->height(), 1.0);
        m_anchorViewportPos = viewportPos;
    }

    void restoreZoomAnchor(double fx, double fy, const QPoint &viewportPos)
    {
        if (!m_scroll || !m_label || m_label->width() <= 0 || m_label->height() <= 0) return;
        const QPoint labelAnchor(qRound(fx * m_label->width()), qRound(fy * m_label->height()));
        const QPoint currentViewportPos = m_label->mapTo(m_scroll->viewport(), labelAnchor);
        const QPoint delta = currentViewportPos - viewportPos;
        if (m_scroll->horizontalScrollBar()) {
            QScrollBar *bar = m_scroll->horizontalScrollBar();
            bar->setValue(bar->value() + delta.x());
        }
        if (m_scroll->verticalScrollBar()) {
            QScrollBar *bar = m_scroll->verticalScrollBar();
            bar->setValue(bar->value() + delta.y());
        }
    }

    void toggleFullscreen()
    {
        if (isFullScreen()) showNormal();
        else showFullScreen();
    }

    void updateDisplayedImage()
    {
        if (!m_label || m_sourceImage.isNull()) return;
        const QImage img = m_histStretch ? histogramStretch(m_sourceImage) : m_sourceImage;
        m_label->setPixmap(QPixmap::fromImage(img));
        const double displayScale = qBound(0.05, m_scale, 5.0);
        m_label->resize(qMax(1, qRound(img.width() * displayScale)),
                        qMax(1, qRound(img.height() * displayScale)));
    }

    static QImage histogramStretch(const QImage &src)
    {
        if (src.isNull()) return src;

        QImage rgb = src.convertToFormat(QImage::Format_RGB32);
        int hist[256] = {0};
        const int w = rgb.width();
        const int h = rgb.height();
        const int total = w * h;
        if (total <= 0) return rgb;

        for (int y = 0; y < h; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                ++hist[qGray(line[x])];
            }
        }

        const int lowTarget = qMax(0, total / 100);
        const int highTarget = qMax(0, total - total / 100);
        int low = 0;
        int acc = 0;
        for (; low < 255; ++low) {
            acc += hist[low];
            if (acc >= lowTarget) break;
        }
        int high = 255;
        acc = 0;
        for (; high > 0; --high) {
            acc += hist[high];
            if (acc >= (total - highTarget)) break;
        }

        if (high <= low + 1) return rgb;

        int lut[256];
        for (int i = 0; i < 256; ++i) {
            if (i <= low) lut[i] = 0;
            else if (i >= high) lut[i] = 255;
            else lut[i] = qBound(0, (i - low) * 255 / (high - low), 255);
        }

        for (int y = 0; y < h; ++y) {
            QRgb *line = reinterpret_cast<QRgb*>(rgb.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb p = line[x];
                line[x] = qRgb(lut[qRed(p)], lut[qGreen(p)], lut[qBlue(p)]);
            }
        }
        return rgb;
    }

    QScrollArea *m_scroll = nullptr;
    QLabel *m_label = nullptr;
    QImage m_sourceImage;
    double m_scale = 1.0;
    std::function<void(double)> m_onScale;
    double m_anchorFx = -1.0;
    double m_anchorFy = -1.0;
    QPoint m_anchorViewportPos;
    bool m_dragging = false;
    QPoint m_lastDragPos;
    bool m_histStretch = false;
    bool m_centerViewportZoom = false;
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
            fullRgbScale = qBound(0.05, fullRgbScale, 5.0);
        }
        emit fullScaledReady(false, fullRgbAngle, fullRgbScale, rgb);
    }

    if (doFullBw) {
        QImage bw = m_cache->extractFullBwWindow(fullBwAngle, 0);
        if (bw.isNull()) bw = m_cache->extractThumbBwWindow(fullBwAngle, 0);
        if (!bw.isNull() && bw.format() == QImage::Format_Indexed8) bw = bw.convertToFormat(QImage::Format_RGB32);
        if (!bw.isNull()) fullBwScale = qBound(0.05, fullBwScale, 5.0);
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
    const AppConfig &cfg = AppConfig::instance();
    m_replayMode = cfg.replayMode;
    if (m_replayMode) {
        setWindowTitle(QStringLiteral("DMX Test - 20260723 - 8s/rev"));
    }
    m_captureLatencyMs = cfg.captureLatencyMs;
    m_captureLatencyRgbMs = cfg.captureLatencyRgbMs;
    m_captureLatencyBwMs = cfg.captureLatencyBwMs;
    m_angleLookup = cfg.angleLookup;
    m_lastAngleDiagMs = 0;

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
    m_panoCache->configureFullSize(cfg.fullWidth, cfg.fullHeight);
    m_panoCache->configureThumbSize(cfg.thumbWidth, cfg.thumbHeight);
    {
        QImage rgb(cfg.thumbWidth, cfg.thumbHeight, QImage::Format_RGB32);
        rgb.fill(Qt::black);
        QImage bw(cfg.thumbWidth, cfg.thumbHeight, QImage::Format_RGB32);
        bw.fill(Qt::black);
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
           QStringLiteral("CONFIG file=%1 loaded=%2%3")
               .arg(cfg.loadedPath.isEmpty() ? QStringLiteral("(none)") : cfg.loadedPath)
               .arg(cfg.loadedFromFile ? 1 : 0)
               .arg(cfg.loadError.isEmpty() ? QString() : QStringLiteral(" error=%1").arg(cfg.loadError)),
           QStringLiteral("#FFD54F"));
    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
           QStringLiteral("BUILD saveRoot=%1 recRoot=%2").arg(resolveSaveRoot(), resolveRecordRoot()),
           QStringLiteral("#FFD54F"));
    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
           QStringLiteral("BUILD logRoot=%1 (file=%2)").arg(resolveLogRoot(),
               m_logFile ? m_logFile->fileName() : QStringLiteral("(disabled)")),
           QStringLiteral("#FFD54F"));
    addLog(QStringLiteral("ANGLE"),
           QStringLiteral("lookup=%1 D=%2ms rgbD=%3ms bwD=%4ms")
               .arg(m_angleLookup ? 1 : 0)
               .arg(m_captureLatencyMs)
               .arg(m_captureLatencyRgbMs)
               .arg(m_captureLatencyBwMs),
           QStringLiteral("#9C27B0"));
    if (m_replayMode) {
        addLog(QStringLiteral("REPLAY"),
               QStringLiteral("test mode ready: click Device Run, fixed 8s/rev, pathPort=%1, skyShrink=%2px")
                   .arg(cfg.pathPort)
                   .arg(cfg.detectSkyShrinkPixels),
               QStringLiteral("#FFD54F"));
    }
    m_pageStack = new QStackedWidget(this);
    setCentralWidget(m_pageStack);
    QWidget *central = new QWidget(m_pageStack);
    m_legacyPage = central;
    m_pageStack->addWidget(central);
    QGridLayout *layout = new QGridLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);

    panoramaView = new PanoramaWidget(this);
    thermalPanoramaView = new PanoramaWidget(this);
    colorRoiView = new AIVideoWidget(this);
    thermalRoiView = new AIVideoWidget(this);
    m_compactTargetRadar = new CompactTargetRadarPanel(this);
    radarView = new RadarWidget(this);
    radarFeedbackView = new AIVideoWidget(this);

    panoramaView->setFixedHeight(80);
    thermalPanoramaView->setFixedHeight(100);
    panoramaView->setShowRuler(false);
    thermalPanoramaView->setShowRuler(true);

    m_angleLabel = new QLabel("0.00°", radarView);
    m_angleLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_angleLabel->setStyleSheet("color: #00FF00; font-family: 'Monospace'; font-size: 18px; font-weight: bold; background-color: rgba(0,0,0,120); padding: 4px; border-radius: 4px;");

    m_lapTimeLabel = new QLabel(u8s("圈速: -- 秒"), radarView);
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
    bottomLayout->setSpacing(8);
    // Keep the legacy widgets alive for existing update paths, but use the
    // unified target panel as the only visible bottom view.
    radarView->hide();
    radarFeedbackView->hide();
    bottomLayout->addWidget(m_compactTargetRadar, 1);
    layout->addLayout(bottomLayout, 2, 0, 1, 2);

    layout->setRowStretch(0, 0);
    layout->setRowStretch(1, 4);
    layout->setRowStretch(2, 2);

    m_targetRadarWindow = new TargetRadarWindow(QString(), m_pageStack);
    connect(m_targetRadarWindow, &TargetRadarWindow::visibleTargetsChanged,
            m_compactTargetRadar, &CompactTargetRadarPanel::setTargets);
    m_targetRadarWindow->clearTargets();
    m_pageStack->addWidget(m_targetRadarWindow);

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
        RoiPopupDialog *dlg = new RoiPopupDialog(this, true);
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
        RoiPopupDialog *dlg = new RoiPopupDialog(this, true);
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
    if (m_replayMode) {
        m_replaySweepTimer = new QTimer(this);
        m_replaySweepTimer->setTimerType(Qt::PreciseTimer);
        m_replaySweepTimer->setInterval(30);
        connect(m_replaySweepTimer, &QTimer::timeout, this, &MainWindow::advanceReplaySweep);
    }

    m_driver = new TurntableDriver(this);
    m_ctrlDialog = new TurntableControlDialog(m_driver, this);
    TurntableControlDialog::Settings turntableSettings;
    turntableSettings.serialPort = cfg.turntableSerialPort;
    turntableSettings.baudRate = cfg.turntableBaudRate;
    turntableSettings.direction = cfg.turntableDirection;
    turntableSettings.speed = cfg.turntableSpeed;
    turntableSettings.orthoEnabled = cfg.turntableOrthoEnabled;
    turntableSettings.orthoLength = cfg.turntableOrthoLength;
    turntableSettings.feedbackEnabled = cfg.turntableFeedbackEnabled;
    m_ctrlDialog->applySettings(turntableSettings);

    QShortcut *shortcutF5 = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(shortcutF5, &QShortcut::activated, this, &MainWindow::onClearUiClicked);
    QShortcut *shortcutCtrlR = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(shortcutCtrlR, &QShortcut::activated, this, &MainWindow::onClearUiClicked);

    connect(m_driver, &TurntableDriver::angleUpdated, this, [=](double realAngle){
        if (m_waitingForRunAngle) {
            m_waitingForRunAngle = false;
            m_zeroAngleInited = true;
            m_zeroAngleRaw = realAngle;
            m_angleHistory.clear();
            sendCommand("TG_OPEN_DEVICE;");
            m_isDeviceOpen = true;
            updateUiState();
            addLog(QStringLiteral("TURNTABLE"),
                   u8s("\xE6\xAD\xA3\xE4\xBA\xA4\xE8\xBE\x93\xE5\x87\xBA\xE5\xB7\xB2\xE7\x94\x9F\xE6\x95\x88\xEF\xBC\x8C\xE5\xB7\xB2\xE6\x8C\x89\xE5\xBD\x93\xE5\x89\x8D\xE9\x9B\xB6\xE7\x82\xB9\xE5\xBC\x80\xE5\xA7\x8B\xE6\x8E\xA5\xE6\x94\xB6\xE5\x9B\xBE\xE5\x83\x8F\xE6\x95\xB0\xE6\x8D\xAE"),
                   QStringLiteral("#6A9955"));
            if (ui->statusbar) {
                ui->statusbar->showMessage(u8s("\xE6\xAD\xA3\xE4\xBA\xA4\xE8\xBE\x93\xE5\x87\xBA\xE5\xB7\xB2\xE7\x94\x9F\xE6\x95\x88\xEF\xBC\x8C\xE5\xBC\x80\xE5\xA7\x8B\xE6\x8E\xA5\xE6\x94\xB6\xE5\x9B\xBE\xE5\x83\x8F\xE6\x95\xB0\xE6\x8D\xAE"), 3000);
            }
        } else if (!m_zeroAngleInited) {
            m_zeroAngleInited = true;
            m_zeroAngleRaw = realAngle;
        }
        const double displayAngle = toRelativeAngle(realAngle);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        m_latestAngle = displayAngle;
        m_angleHistory.add(nowMs, displayAngle);
        if (m_prevRadarSweepAngle >= 0.0) {
            pruneDetectedRadarTargets(nowMs, true);
        }
        m_prevRadarSweepAngle = displayAngle;
        radarView->setCurrentAngle(displayAngle);
        if (m_compactTargetRadar) m_compactTargetRadar->setScanAngle(displayAngle);
        if (m_targetRadarWindow) m_targetRadarWindow->setScanAngle(displayAngle);
        m_angleLabel->setText(QString("%1°").arg(displayAngle, 0, 'f', 2));
        if (m_colorThread) m_colorThread->setCurrentAngle(displayAngle);
        if (m_thermalThread) m_thermalThread->setCurrentAngle(displayAngle);

        if (m_recordPendingZero) {
            if (!m_recordPendingHasAngle) {
                m_recordPendingPrevAngle = displayAngle;
                m_recordPendingHasAngle = true;
            } else if (crossedZero(m_recordPendingPrevAngle, displayAngle)) {
                startRecordingNowAfterZero();
            } else {
                m_recordPendingPrevAngle = displayAngle;
            }
        }

        static bool speedInit = false;
        static double prevA = 0.0;
        static qint64 prevMs = 0;
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
        m_lapTimeLabel->setText(u8s("圈速: %1 秒").arg(lapTime, 0, 'f', 2));
        addLog(QStringLiteral("TURNTABLE"),
               u8s("实测圈速=%1秒").arg(lapTime, 0, 'f', 2),
               QStringLiteral("#569CD6"));
    });

    // ====================================================================
    m_cmdSocket = new QUdpSocket(this);
    m_replySocket = new QUdpSocket(this);

    addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), u8s("\xE6\x8C\x87\xE4\xBB\xA4\xE6\x8E\xA7\xE5\x88\xB6\xE5\x87\x86\xE5\xA4\x87\xE5\xB0\xB1\xE7\xBB\xAA\xEF\xBC\x8C\xE7\x9B\xAE\xE6\xA0\x87\x3A\x20\x25\x31").arg(cfg.deviceIp), "#569CD6");

    if (m_replySocket->bind(QHostAddress::AnyIPv4, cfg.cmdPortReply, QUdpSocket::ShareAddress)) {
        qDebug() << ">>> [UDP] bind" << cfg.cmdPortReply << "ok";
        addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), QStringLiteral("UDP reply bind %1 ok").arg(cfg.cmdPortReply), "#6A9955");
    } else {
        qDebug() << ">>> [UDP] bind" << cfg.cmdPortReply << "failed";
        addLog(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), QStringLiteral("UDP reply bind %1 failed").arg(cfg.cmdPortReply), "#F44336");
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
    connect(m_colorThread, &VideoThread::candidateDetected, this, &MainWindow::onCandidateDetected, Qt::QueuedConnection);
    connect(m_thermalThread, &VideoThread::candidateDetected, this, &MainWindow::onCandidateDetected, Qt::QueuedConnection);

#ifdef DMX_ADVANCED_DETECTION
    m_directYoloManager = new DirectYoloManager(this);
    connect(m_directYoloManager, &DirectYoloManager::logRequested, this, &MainWindow::addLog);
    connect(m_directYoloManager,
            &DirectYoloManager::candidateDetected,
            this,
            &MainWindow::onCandidateDetected,
            Qt::QueuedConnection);
    connect(m_directYoloManager,
            &DirectYoloManager::staticClutterDetected,
            this,
            &MainWindow::onStaticClutterDetected,
            Qt::QueuedConnection);
    connect(m_colorThread,
            &VideoThread::directYoloFrameReady,
            m_directYoloManager,
            &DirectYoloManager::submitFrame,
            Qt::QueuedConnection);
    connect(m_thermalThread,
            &VideoThread::directYoloFrameReady,
            m_directYoloManager,
            &DirectYoloManager::submitFrame,
            Qt::QueuedConnection);
#endif

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
    if (m_replayMode && qEnvironmentVariableIntValue("DMX_REPLAY_AUTOSTART") == 1) {
        QTimer::singleShot(0, this, &MainWindow::onActionOpenDevice);
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    QMainWindow::mousePressEvent(event);
}

MainWindow::~MainWindow()
{
    stopRecordingNow();
    if(m_ctrlDialog) m_ctrlDialog->stopAndDisableOrtho();
    else if(m_driver) m_driver->stop();
    if(m_driver) m_driver->closePort();
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
    const AppConfig &cfg = AppConfig::instance();
    QByteArray data = cmd.toUtf8();
    m_cmdSocket->writeDatagram(data, QHostAddress(cfg.deviceIp), cfg.cmdPortSend);
    qDebug() << ">>> [UDP TX] ->" << cfg.deviceIp << ":" << cfg.cmdPortSend << "|" << cmd;
    addLog(QStringLiteral("CMD(%1)").arg(cfg.cmdPortSend), cmd, "#569CD6");
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

    m_mainToolBar->setMinimumHeight(50);
    m_mainToolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_mainToolBar->setStyleSheet(
        "QToolBar { spacing: 6px; padding: 4px 6px; }"
        "QToolButton { min-height: 34px; padding: 4px 8px; }");

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
    m_actTargetRadar = new QAction(u8s("\xE7\x9B\xAE\xE6\xA0\x87\xE9\x9B\xB7\xE8\xBE\xBE"), this);
    m_actTargetRadar->setCheckable(true);
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
    m_mainToolBar->addAction(m_actTargetRadar);

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
    connect(m_actTargetRadar, &QAction::triggered, this, &MainWindow::onOpenTargetRadarWindow);
    connect(actOpenTurntable, &QAction::triggered, this, [=](){ m_ctrlDialog->show(); });
    connect(m_actExit, &QAction::triggered, this, &MainWindow::close);

    updateUiState();
}

void MainWindow::onOpenTargetRadarWindow()
{
    if (!m_pageStack || !m_legacyPage || !m_targetRadarWindow) return;

    const bool showRadar = m_actTargetRadar && m_actTargetRadar->isChecked();
    if (showRadar) {
        m_pageStack->setCurrentWidget(m_targetRadarWindow);
        updateTargetRadarBackground();
        m_targetRadarWindow->setScanAngle(m_latestAngle);
        m_actTargetRadar->setText(u8s("\xE8\xBF\x94\xE5\x9B\x9E\xE4\xB8\xBB\xE7\x95\x8C\xE9\x9D\xA2"));
    } else {
        m_pageStack->setCurrentWidget(m_legacyPage);
        m_actTargetRadar->setText(u8s("\xE7\x9B\xAE\xE6\xA0\x87\xE9\x9B\xB7\xE8\xBE\xBE"));
    }
}

void MainWindow::updateTargetRadarBackground()
{
    if (!m_targetRadarWindow && !m_compactTargetRadar) return;
    if (m_uiThumbRgb.isNull() && m_uiThumbBw.isNull()) return;

    // m_uiThumbBw is already aligned by PanoramaCache, so neither view rotates it again.
    if (m_compactTargetRadar && m_compactTargetRadar->isVisible()) {
        m_compactTargetRadar->setPanorama(
            !m_uiThumbBw.isNull() ? m_uiThumbBw : m_uiThumbRgb, false);
    }
    if (m_targetRadarWindow && m_targetRadarWindow->isVisible()) {
#ifdef DMX_ADVANCED_DETECTION
        m_targetRadarWindow->setLivePanoramas(m_uiThumbRgb, m_uiThumbBw, false);
#else
        if (!m_uiThumbBw.isNull()) m_targetRadarWindow->setLivePanorama(m_uiThumbBw, false);
#endif
    }
}

void MainWindow::updateUiState()
{
    const bool opening = m_waitingForRunAngle;
    m_actOpenDevice->setEnabled(!m_isDeviceOpen && !opening);
    m_actCloseDevice->setEnabled(m_isDeviceOpen || opening);
    m_actSavePng->setEnabled(m_isDeviceOpen);
    m_actSaveJpg->setEnabled(m_isDeviceOpen);
    m_actSaveVideo->setEnabled(m_isDeviceOpen);
    m_actStopCapture->setEnabled(m_isDeviceOpen);
    if (m_actRecord) {
        m_actRecord->setEnabled(m_isDeviceOpen || m_isRecording || m_recordPendingZero);
        updateRecordActionText();
    }

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
        updateRecordActionText();
        return;
    }
    if (!m_isDeviceOpen) {
        m_actRecord->setChecked(false);
        updateRecordActionText();
        return;
    }

    const bool enable = m_actRecord->isChecked();
    if (enable) {
        requestRecordingStartAtNextZero();
        return;
    }
    stopRecordingNow();
}

void MainWindow::requestRecordingStartAtNextZero()
{
    if (!m_recordWorker || !m_isDeviceOpen) {
        if (m_actRecord) m_actRecord->setChecked(false);
        updateRecordActionText();
        return;
    }
    if (m_isRecording || m_recordPendingZero) {
        if (m_actRecord) m_actRecord->setChecked(true);
        updateRecordActionText();
        return;
    }

    m_recordPendingZero = true;
    m_recordPendingHasAngle = m_zeroAngleInited;
    m_recordPendingPrevAngle = m_latestAngle;
    if (m_colorThread) m_colorThread->setRecordingEnabled(false);
    if (m_thermalThread) m_thermalThread->setRecordingEnabled(false);
    if (m_actRecord) m_actRecord->setChecked(true);
    updateRecordActionText();
    addLog(QStringLiteral("REC"), u8s("\xE7\xAD\x89\xE5\xBE\x85\xE8\xBD\xAC\xE5\x8F\xB0\xE8\xBF\x87\xE9\x9B\xB6\xE7\x82\xB9\xE5\x90\x8E\xE5\xBC\x80\xE5\xA7\x8B\xE5\xBD\x95\xE5\x88\xB6"), QStringLiteral("#FFD54F"));
    if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE7\xAD\x89\xE5\xBE\x85\xE8\xBD\xAC\xE5\x8F\xB0\xE8\xBF\x87\xE9\x9B\xB6\xE7\x82\xB9\xE5\x90\x8E\xE5\xBC\x80\xE5\xA7\x8B\xE5\xBD\x95\xE5\x88\xB6"), 3000);
}

void MainWindow::startRecordingNowAfterZero()
{
    if (!m_recordWorker || !m_isDeviceOpen || !m_recordPendingZero) return;

    m_recordPendingZero = false;
    m_recordPendingHasAngle = false;
    m_isRecording = true;

    QMetaObject::invokeMethod(
        m_recordWorker,
        "startRecording",
        Qt::BlockingQueuedConnection,
        Q_ARG(QString, resolveRecordRoot()),
        Q_ARG(int, AppConfig::instance().recordRollMinutes));
    if (m_colorThread) m_colorThread->setRecordingEnabled(true);
    if (m_thermalThread) m_thermalThread->setRecordingEnabled(true);
    if (m_actRecord) m_actRecord->setChecked(true);
    updateRecordActionText();
    addLog(QStringLiteral("REC"), u8s("\xE5\xBD\x95\xE5\x88\xB6\xE5\xB7\xB2\xE5\x9C\xA8\xE8\xBF\x87\xE9\x9B\xB6\xE7\x82\xB9\xE5\x90\x8E\xE5\xBC\x80\xE5\xA7\x8B"), QStringLiteral("#6A9955"));
    if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE5\xBD\x95\xE5\x88\xB6\xE5\xB7\xB2\xE5\x9C\xA8\xE8\xBF\x87\xE9\x9B\xB6\xE7\x82\xB9\xE5\x90\x8E\xE5\xBC\x80\xE5\xA7\x8B"), 3000);
}

void MainWindow::stopRecordingNow()
{
    const bool wasPending = m_recordPendingZero;
    const bool wasRecording = m_isRecording;
    m_recordPendingZero = false;
    m_recordPendingHasAngle = false;
    m_isRecording = false;

    if (m_colorThread) m_colorThread->setRecordingEnabled(false);
    if (m_thermalThread) m_thermalThread->setRecordingEnabled(false);
    if (wasRecording && m_recordWorker) {
        QMetaObject::invokeMethod(m_recordWorker, "stopRecording", Qt::QueuedConnection);
    }
    if (m_actRecord) m_actRecord->setChecked(false);
    updateRecordActionText();

    if (wasPending) {
        addLog(QStringLiteral("REC"), u8s("\xE5\x8F\x96\xE6\xB6\x88\xE7\xAD\x89\xE5\xBE\x85\xE8\xBF\x87\xE9\x9B\xB6\xE5\xBD\x95\xE5\x88\xB6"), QStringLiteral("#569CD6"));
    } else if (wasRecording) {
        addLog(QStringLiteral("REC"), QStringLiteral("Stop"), QStringLiteral("#569CD6"));
    }
}

void MainWindow::updateRecordActionText()
{
    if (!m_actRecord) return;
    if (m_recordPendingZero) {
        m_actRecord->setText(u8s("\xE7\xAD\x89\xE5\xBE\x85\xE8\xBF\x87\xE9\x9B\xB6\xE5\xBD\x95\xE5\x88\xB6"));
    } else if (m_isRecording) {
        m_actRecord->setText(u8s("\xE5\x81\x9C\xE6\xAD\xA2\xE5\xBD\x95\xE5\x88\xB6"));
    } else {
        m_actRecord->setText(u8s("\xE5\xBC\x80\xE5\xA7\x8B\xE5\xBD\x95\xE5\x88\xB6"));
    }
}

bool MainWindow::crossedZero(double prevAngleDeg, double currentAngleDeg)
{
    return qAbs(currentAngleDeg - prevAngleDeg) > 180.0;
}

void MainWindow::updateReplaySweep(double angleDeg)
{
    if (!m_replayMode) return;

    const double displayAngle = normalizeReplayAngle(angleDeg);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_latestAngle = displayAngle;
    if (m_prevRadarSweepAngle >= 0.0) {
        pruneDetectedRadarTargets(nowMs, true);
    }
    m_prevRadarSweepAngle = displayAngle;
    if (radarView) radarView->setCurrentAngle(displayAngle);
    if (m_compactTargetRadar) m_compactTargetRadar->setScanAngle(displayAngle);
    if (m_targetRadarWindow) m_targetRadarWindow->setScanAngle(displayAngle);
    if (m_angleLabel) {
        m_angleLabel->setText(QStringLiteral("%1").arg(displayAngle, 0, 'f', 2) + QChar(0x00B0));
    }
    if (m_colorThread) m_colorThread->setCurrentAngle(displayAngle);
    if (m_thermalThread) m_thermalThread->setCurrentAngle(displayAngle);
}

void MainWindow::advanceReplaySweep()
{
    if (!m_replayMode || !m_isDeviceOpen || !m_replaySweepClock.isValid()) return;

    const qint64 elapsedMs = m_replaySweepClock.restart();
    if (elapsedMs <= 0) return;

    const double degreesPerSecond = 360.0 / 8.0;
    m_replaySweepAngleDeg = normalizeReplayAngle(
        m_replaySweepAngleDeg - degreesPerSecond * ((double)elapsedMs / 1000.0));
    updateReplaySweep(m_replaySweepAngleDeg);
}

bool MainWindow::scanCrossedAngle(double prevAngleDeg, double currentAngleDeg, double targetAngleDeg) const
{
    auto normalize = [](double a) {
        while (a < 0.0) a += 360.0;
        while (a >= 360.0) a -= 360.0;
        return a;
    };

    const double prev = normalize(prevAngleDeg);
    const double current = normalize(currentAngleDeg);
    const double target = normalize(targetAngleDeg);
    double delta = current - prev;
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    if (qAbs(delta) < 0.001) return false;

    if (delta > 0.0) {
        double distance = target - prev;
        while (distance <= 0.0) distance += 360.0;
        return distance <= delta;
    }

    double distance = prev - target;
    while (distance <= 0.0) distance += 360.0;
    return distance <= -delta;
}

void MainWindow::pruneDetectedRadarTargets(qint64 nowMs, bool removeScanned)
{
    const AppConfig &cfg = AppConfig::instance();
    bool changed = false;
    if (m_detectTargetMs.size() != m_simTargets.size()) {
        m_simTargets.clear();
        m_detectTargetMs.clear();
        changed = true;
    }

    const qint64 cutoff = nowMs - cfg.detectRadarHoldMs;
    const qint64 minVisibleMs = qMax<qint64>(250, qMin<qint64>(1000, cfg.detectRadarHoldMs));
    for (int i = m_simTargets.size() - 1; i >= 0; --i) {
        if (i >= m_detectTargetMs.size()) continue;
        const qint64 ageMs = nowMs - m_detectTargetMs[i];
        const bool expired = m_detectTargetMs[i] < cutoff;
        const bool scanned = removeScanned && ageMs >= minVisibleMs &&
                             scanCrossedAngle(m_prevRadarSweepAngle, m_latestAngle, m_simTargets[i].angle);
        if (expired || scanned) {
            m_simTargets.remove(i);
            m_detectTargetMs.remove(i);
            changed = true;
        }
    }

    if (changed && radarView) radarView->setTargets(m_simTargets);
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
    if (m_isDeviceOpen || m_waitingForRunAngle) return;

    if (m_replayMode) {
        m_isDeviceOpen = true;
        m_waitingForRunAngle = false;
        m_zeroAngleInited = false;
        m_prevRadarSweepAngle = -1.0;
        m_angleHistory.clear();
        m_replaySweepClock.start();
        updateReplaySweep(m_replaySweepAngleDeg);
        if (m_replaySweepTimer) m_replaySweepTimer->start();
        sendCommand(QStringLiteral("DMX_REPLAY_START;"));
        updateUiState();
        m_lapTimeLabel->setText(u8s("\xE5\x9C\x88\xE9\x80\x9F\x3A\x20\x38\x2E\x30\x30\x20\xE7\xA7\x92\x20\x28\xE5\x9B\x9E\xE6\x94\xBE\x29"));
        addLog(QStringLiteral("REPLAY"),
               u8s("\xE5\xB7\xB2\xE5\x90\xAF\xE5\x8A\xA8\x20\x32\x30\x32\x36\x30\x37\x32\x33\x20\xE5\x9F\xBA\xE5\x87\x86\xE6\x95\xB0\xE6\x8D\xAE\xE5\x9B\x9E\xE6\x94\xBE\xEF\xBC\x8C\xE5\x9B\xBA\xE5\xAE\x9A\x20\x38\x20\xE7\xA7\x92\x2F\xE5\x9C\x88"),
               QStringLiteral("#6A9955"));
        if (ui->statusbar) {
            ui->statusbar->showMessage(
                u8s("\xE5\x9B\x9E\xE6\x94\xBE\xE5\xB7\xB2\xE5\x90\xAF\xE5\x8A\xA8\xEF\xBC\x9A\x32\x30\x32\x36\x30\x37\x32\x33\xEF\xBC\x8C\x38\x20\xE7\xA7\x92\x2F\xE5\x9C\x88"),
                4000);
        }
        return;
    }

    QString turntableErr;
    TurntableControlDialog::Settings s;
    if (m_ctrlDialog && m_ctrlDialog->runWithStartupSettings(&s, &turntableErr)) {
        const QString directionText = (s.direction == QStringLiteral("left")) ? u8s("\xE5\xB7\xA6\xE8\xBD\xAC") : u8s("\xE5\x8F\xB3\xE8\xBD\xAC");
        const QString gearText = (s.speed == 43) ? u8s("8秒/圈") : u8s("自定义档位");
        m_waitingForRunAngle = true;
        m_isDeviceOpen = false;
        m_zeroAngleInited = false;
        m_prevRadarSweepAngle = -1.0;
        m_angleHistory.clear();
        m_lastAngleDiagMs = 0;
        updateUiState();
        addLog(QStringLiteral("TURNTABLE"),
               u8s("自动启动 目标档位=%1 speed=%2").arg(gearText).arg(s.speed),
               QStringLiteral("#6A9955"));
        addLog(QStringLiteral("TURNTABLE"),
               u8s("\xE8\xBF\x90\xE8\xA1\x8C\x20\xE6\x96\xB9\xE5\x90\x91\x3D\x25\x31\x20\xE9\x80\x9F\xE5\xBA\xA6\x3D\x25\x32\x20\xE4\xB8\xB2\xE5\x8F\xA3\x3D\x25\x33\x20\xE6\xB3\xA2\xE7\x89\xB9\xE7\x8E\x87\x3D\x25\x34\x20\xE6\xAD\xA3\xE4\xBA\xA4\xE8\xBE\x93\xE5\x87\xBA\x3D\xE7\xAD\x89\xE5\xBE\x85\xE8\xBF\x87\xE9\x9B\xB6\xE5\xBC\x80\xE5\x90\xAF\x20\xE8\xA7\x92\xE5\xBA\xA6\xE5\x9B\x9E\xE4\xBC\xA0\x3D\x25\x35")
                   .arg(directionText)
                   .arg(s.speed)
                   .arg(s.serialPort.isEmpty() ? u8s("\x28\xE6\x9C\xAA\xE9\x80\x89\xE6\x8B\xA9\x29") : s.serialPort)
                   .arg(s.baudRate)
                   .arg(s.feedbackEnabled ? 1 : 0),
               QStringLiteral("#6A9955"));
        if (ui->statusbar) {
            ui->statusbar->showMessage(u8s("\xE8\xBD\xAC\xE5\x8F\xB0\xE5\xB7\xB2\xE5\x90\xAF\xE5\x8A\xA8\xEF\xBC\x8C\xE7\xAD\x89\xE5\xBE\x85\xE6\xAD\xA3\xE4\xBA\xA4\xE8\xBE\x93\xE5\x87\xBA\xE8\xBF\x87\xE9\x9B\xB6\xE7\x82\xB9\xE5\x90\x8E\xE6\x8E\xA5\xE6\x94\xB6\xE5\x9B\xBE\xE5\x83\x8F\xE6\x95\xB0\xE6\x8D\xAE"), 5000);
        }
        return;
    } else if (!turntableErr.isEmpty()) {
        addLog(QStringLiteral("TURNTABLE"), u8s("\xE8\x87\xAA\xE5\x8A\xA8\xE5\x90\xAF\xE5\x8A\xA8\xE5\xA4\xB1\xE8\xB4\xA5\x3A\x20\x25\x31").arg(turntableErr), QStringLiteral("#F44336"));
        if (ui->statusbar) {
            ui->statusbar->showMessage(u8s("\xE8\xBD\xAC\xE5\x8F\xB0\xE6\x9C\xAA\xE5\x90\xAF\xE5\x8A\xA8\x3A\x20\x25\x31").arg(turntableErr), 5000);
        }
    }
    updateUiState();
}

void MainWindow::onActionCloseDevice()
{
    stopRecordingNow();
    if (m_replayMode) {
        if (m_isDeviceOpen) sendCommand(QStringLiteral("DMX_REPLAY_STOP;"));
        m_isDeviceOpen = false;
        m_waitingForRunAngle = false;
        if (m_replaySweepTimer) m_replaySweepTimer->stop();
        m_replaySweepClock.invalidate();
        updateUiState();
        addLog(QStringLiteral("REPLAY"),
               u8s("\xE5\x9B\x9E\xE6\x94\xBE\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C\xEF\xBC\x8C\xE5\x86\x8D\xE6\xAC\xA1\xE7\x82\xB9\xE5\x87\xBB\xE8\xAE\xBE\xE5\xA4\x87\xE8\xBF\x90\xE8\xA1\x8C\xE5\x8F\xAF\xE7\xBB\xA7\xE7\xBB\xAD"),
               QStringLiteral("#569CD6"));
        if (ui->statusbar) {
            ui->statusbar->showMessage(
                u8s("\xE5\x9B\x9E\xE6\x94\xBE\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C"),
                3000);
        }
        return;
    }
    if (m_isDeviceOpen || m_waitingForRunAngle) {
        sendCommand("TG_CLOSE_DEVICE;");
    }
    m_waitingForRunAngle = false;
    if (m_ctrlDialog) m_ctrlDialog->stopAndDisableOrtho();
    else if (m_driver) {
        m_driver->stop();
        QTimer::singleShot(40, this, [this]() {
            if (m_driver && m_driver->isOpen()) m_driver->disableOrtho();
        });
    }
    m_isDeviceOpen = false;
    m_zeroAngleInited = false;
    m_angleHistory.clear();
    updateUiState();
    addLog(QStringLiteral("TURNTABLE"),
           u8s("\xE5\x81\x9C\xE6\xAD\xA2\xE8\xAE\xBE\xE5\xA4\x87\xEF\xBC\x9A\xE8\xBD\xAC\xE5\x8F\xB0\xE5\xB7\xB2\xE5\x81\x9C\xE6\xAD\xA2\xEF\xBC\x8C\xE6\xAD\xA3\xE4\xBA\xA4\xE8\xBE\x93\xE5\x87\xBA\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD"),
           QStringLiteral("#569CD6"));
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
    const int previewJpegQuality = AppConfig::instance().previewJpegQuality;
    const int previewWidth = AppConfig::instance().previewWidth;
    addLog(u8s("\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE5\x9B\xBE"), u8s("\xE5\xB7\xB2\xE5\x85\xA5\xE9\x98\x9F\x3A\x20") + outDir, "#569CD6");
    if (ui->statusbar) ui->statusbar->showMessage(u8s("\xE5\xB7\xB2\xE5\x85\xA5\xE9\x98\x9F\xE4\xBF\x9D\xE5\xAD\x98\xE5\x85\xA8\xE6\x99\xAF\x2E\x2E\x2E"), 2000);
    emit savePanoramaRequested(outDir, previewJpegQuality, previewWidth);
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
        const AppConfig &cfg = AppConfig::instance();
        QImage rgb(cfg.thumbWidth, cfg.thumbHeight, QImage::Format_RGB32);
        rgb.fill(Qt::black);
        QImage bw(cfg.thumbWidth, cfg.thumbHeight, QImage::Format_RGB32);
        bw.fill(Qt::black);
        m_uiThumbRgb = rgb;
        m_uiThumbBw = bw;
        panoramaView->updateImage(m_uiThumbRgb);
        thermalPanoramaView->updateImage(m_uiThumbBw);
    }

    QImage blackImg(1024, 2048, QImage::Format_RGB32);
    blackImg.fill(Qt::black);
    colorRoiView->updateImage(blackImg);
    thermalRoiView->updateImage(blackImg);
    radarFeedbackView->updateImage(blackImg);
    if (m_targetRadarWindow) m_targetRadarWindow->clearTargets();
    updateTargetRadarBackground();

    // NOTE: intentionally do NOT reset the zero-angle reference here. Resetting it
    // would re-anchor 0deg to wherever the turntable happens to be at clear time,
    // shifting all panorama content (same object lands at a different position
    // after a clear). Keep the original zero so placement stays stable.
    m_lastColorRoi = QImage();
    m_lastThermalRoi = QImage();
    if(m_logBrowser) m_logBrowser->clear();

    for(auto& target : m_simTargets) { target.isDetected = false; }
    m_detectTargetMs.clear();
    m_prevRadarSweepAngle = -1.0;
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
    if (!AppConfig::instance().detectEnabled && (nowMs - m_lastDetectMs) >= 120) {
        m_lastDetectMs = nowMs;
        checkTargetDetection(m_latestAngle);
    }
    if (!AppConfig::instance().detectEnabled && m_prevCheckAngle > 300.0 && m_latestAngle < 60.0) {
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
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool radarPageVisible = m_targetRadarWindow && m_targetRadarWindow->isVisible();
    const bool compactRadarVisible = m_compactTargetRadar && m_compactTargetRadar->isVisible();
    if ((radarPageVisible || compactRadarVisible)
        && (!m_uiThumbRgb.isNull() || !m_uiThumbBw.isNull()) &&
        (nowMs - m_lastTargetRadarBgMs) >= 250) {
        m_lastTargetRadarBgMs = nowMs;
        updateTargetRadarBackground();
    }
}

void MainWindow::onPathReceived(const QString &type, const QString &path, const QString &sender, qint64 rxMs)
{
    const QString t = type.trimmed().toUpper();
    if (!m_isDeviceOpen || m_waitingForRunAngle) return;

    double angleDeg = -1.0;
    if (m_replayMode) {
        quint64 fileIndex = 0;
        if (parseReplayFileIndex(path, &fileIndex)) {
            angleDeg = replayFrameAngle(fileIndex);
            if (t == QStringLiteral("BW") || t == QStringLiteral("GRAY")) {
                m_replaySweepAngleDeg = angleDeg;
                m_replaySweepClock.start();
                updateReplaySweep(angleDeg);
                if (!m_replayMappingLogged) {
                    m_replayMappingLogged = true;
                    addLog(QStringLiteral("REPLAY"),
                           QStringLiteral("frame map: index=0,1,2 -> angle=0,337.5,315; "
                                          "direction=left; pixel=CCW90+mirrorH; radar=30ms smooth"),
                           QStringLiteral("#FFD54F"));
                }
            }
        }
    } else {
        angleDeg = m_zeroAngleInited ? m_latestAngle : -1.0;
    }
    qint64 latencyMs = m_captureLatencyRgbMs;
    if (t == "BW" || t == "GRAY") latencyMs = m_captureLatencyBwMs;

    bool lookupClamped = false;
    qint64 lookupBracketMs = 0;
    bool lookupOk = false;
    if (!m_replayMode && m_zeroAngleInited && m_angleLookup && !m_angleHistory.empty()) {
        double looked = -1.0;
        lookupOk = m_angleHistory.angleAt(rxMs - latencyMs, &looked, &lookupClamped, &lookupBracketMs);
        if (lookupOk && looked >= 0.0) angleDeg = looked;
    }

    const qint64 nowD = QDateTime::currentMSecsSinceEpoch();
    if (!m_replayMode && m_angleLookup && (nowD - m_lastAngleDiagMs) > 1000) {
        m_lastAngleDiagMs = nowD;
        addLog(QStringLiteral("ANGLE"),
            QString("type=%1 recv=%2 capture=%3 D=%4ms hist=%5 recent1s=%6 bracket=%7ms clamped=%8 ok=%9")
                .arg(t)
                .arg(m_latestAngle, 0, 'f', 1)
                .arg(angleDeg, 0, 'f', 1)
                .arg(latencyMs)
                .arg(m_angleHistory.size())
                .arg(m_angleHistory.recentCount(nowD, 1000))
                .arg(lookupBracketMs)
                .arg(lookupClamped ? 1 : 0)
                .arg(lookupOk ? 1 : 0),
            "#9C27B0");
    }

    if (t == "RGB") {
        if (m_colorThread) m_colorThread->enqueuePath(t, path, sender, angleDeg, rxMs);
        return;
    }
    if (t == "BW" || t == "GRAY") {
        if (m_thermalThread) m_thermalThread->enqueuePath(t, path, sender, angleDeg, rxMs);
        return;
    }
}

void MainWindow::onCandidateDetected(const QString &stream, double angle, int panoX, int panoY, double score, const QString &cropPath,
                                     int roiBoxX1, int roiBoxY1, int roiBoxX2, int roiBoxY2,
                                     const QString &className)
{
    const AppConfig &cfg = AppConfig::instance();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    pruneDetectedRadarTargets(nowMs, false);
    replaceCandidateCropFromPanorama(stream, panoX, panoY, cropPath);

    bool manualFeedbackDropped = false;
    if (m_targetRadarWindow) {
        TargetRecord rec;
        rec.id = QStringLiteral("T-%1").arg(++m_targetRadarSeq, 4, 10, QLatin1Char('0'));
        rec.time = QDateTime::currentDateTime();
        rec.firstTime = rec.time;
        rec.lastTime = rec.time;
        rec.imagePath = cropPath;
        rec.stream = stream.trimmed().toUpper();
        rec.className = className.trimmed().toLower();
        rec.state = QStringLiteral("new");
        rec.azimuthDeg = angle;
        rec.confidence = qBound(0.0, score / 255.0, 1.0);
        rec.classConfidence = rec.confidence;
        rec.score = score;
        rec.trackAgeSec = 0.0;
        rec.hits = 1;
        rec.panoX = panoX;
        rec.panoY = panoY;
        rec.frameX = 0;
        rec.frameY = 0;
        const int panoramaHeight = qMax(2, cfg.fullHeight);
        const double yNorm = qBound(0.0,
            static_cast<double>(panoY) / static_cast<double>(panoramaHeight - 1),
            1.0);
        rec.elevationAngleDeg = (0.5 - yNorm) * cfg.cameraVerticalFovDeg;
        rec.hasElevationAngle = true;
        rec.hasRoiBox = (roiBoxX2 > roiBoxX1 && roiBoxY2 > roiBoxY1);
        rec.roiBoxX1 = roiBoxX1;
        rec.roiBoxY1 = roiBoxY1;
        rec.roiBoxX2 = roiBoxX2;
        rec.roiBoxY2 = roiBoxY2;
        const QString association = m_targetRadarWindow->addOrUpdateTarget(rec);
        manualFeedbackDropped = association.startsWith(QStringLiteral("manual_feedback_drop"));
        if (!association.isEmpty()) {
            addLog(
                QStringLiteral("TARGET_ASSOC"),
                association,
                manualFeedbackDropped
                    ? QStringLiteral("#90A4AE")
                    : association.startsWith(QStringLiteral("merge"))
                    ? QStringLiteral("#6A9955")
                    : QStringLiteral("#569CD6"));
        }
    }
    if (manualFeedbackDropped) return;

    bool merged = false;
    for (int i = 0; i < m_simTargets.size() && i < m_detectTargetMs.size(); ++i) {
        double diff = qAbs(double(m_simTargets[i].angle) - angle);
        if (diff > 180.0) diff = 360.0 - diff;
        if (diff <= 2.0) {
            m_simTargets[i].angle = (int)qRound(angle);
            m_simTargets[i].isDetected = true;
            m_detectTargetMs[i] = nowMs;
            merged = true;
            break;
        }
    }
    if (!merged) {
        RadarTarget target((int)qRound(angle));
        target.isDetected = true;
        m_simTargets.append(target);
        m_detectTargetMs.append(nowMs);
    }
    while (m_simTargets.size() > cfg.detectMaxRadarTargets && !m_simTargets.isEmpty()) {
        m_simTargets.remove(0);
        if (!m_detectTargetMs.isEmpty()) m_detectTargetMs.remove(0);
    }
    radarView->setTargets(m_simTargets);

    addLog(QStringLiteral("DETECT_UI"),
           QStringLiteral("%1 class=%2 angle=%3 x=%4 y=%5 score=%6 crop=%7")
               .arg(stream)
               .arg(className.isEmpty() ? QStringLiteral("unknown") : className)
               .arg(angle, 0, 'f', 1)
               .arg(panoX)
               .arg(panoY)
               .arg(score, 0, 'f', 1)
               .arg(cropPath),
           QStringLiteral("#FF5252"));

    if (m_panoCache && radarFeedbackView) {
        const QString s = stream.trimmed().toUpper();
        QImage feedback = (s == QStringLiteral("BW"))
            ? m_panoCache->extractFullBwSliceByAngle(angle, true)
            : m_panoCache->extractFullRgbSliceByAngle(angle);
        if (feedback.isNull()) {
            feedback = m_panoCache->extractFullRgbSliceByAngle(angle);
        }
        if (!feedback.isNull()) {
            radarFeedbackView->updateImage(feedback);
        }
    }
}

void MainWindow::onStaticClutterDetected(const QString &stream,
                                         const QString &className,
                                         int panoX,
                                         int panoY,
                                         int trackId,
                                         int stableHits,
                                         double contextScore,
                                         const QString &cropPath)
{
    replaceCandidateCropFromPanorama(stream, panoX, panoY, cropPath);
    if (m_targetRadarWindow) {
        m_targetRadarWindow->suppressStaticClutter(
            stream,
            className,
            panoX,
            panoY,
            trackId,
            stableHits,
            contextScore);
    }
}

void MainWindow::replaceCandidateCropFromPanorama(const QString &stream,
                                                   int panoX,
                                                   int panoY,
                                                   const QString &cropPath)
{
    if (!m_panoCache || cropPath.trimmed().isEmpty()) return;

    const AppConfig &cfg = AppConfig::instance();
    const int cropSize = qMax(32, cfg.detectCropSize);
    const QString normalizedStream = stream.trimmed().toUpper();
    QImage crop = normalizedStream == QStringLiteral("BW") || normalizedStream == QStringLiteral("GRAY")
        ? m_panoCache->extractFullBwCrop(panoX, panoY, cropSize)
        : m_panoCache->extractFullRgbCrop(panoX, panoY, cropSize);
    if (crop.isNull()) {
        addLog(QStringLiteral("CROP"),
               QStringLiteral("panorama crop unavailable stream=%1 x=%2 y=%3 path=%4")
                   .arg(normalizedStream)
                   .arg(panoX)
                   .arg(panoY)
                   .arg(cropPath),
               QStringLiteral("#FFB74D"));
        return;
    }

    QSaveFile output(cropPath);
    if (!output.open(QIODevice::WriteOnly)) {
        addLog(QStringLiteral("CROP"),
               QStringLiteral("panorama crop open failed: %1 error=%2")
                   .arg(cropPath, output.errorString()),
               QStringLiteral("#F44336"));
        return;
    }

    QImageWriter writer(&output, "jpg");
    writer.setQuality(cfg.detectJpegQuality);
    if (!writer.write(crop)) {
        output.cancelWriting();
        addLog(QStringLiteral("CROP"),
               QStringLiteral("panorama crop write failed: %1 error=%2")
                   .arg(cropPath, writer.errorString()),
               QStringLiteral("#F44336"));
        return;
    }
    if (!output.commit()) {
        addLog(QStringLiteral("CROP"),
               QStringLiteral("panorama crop commit failed: %1 error=%2")
                   .arg(cropPath, output.errorString()),
               QStringLiteral("#F44336"));
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
