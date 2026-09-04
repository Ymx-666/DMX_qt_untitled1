#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QUdpSocket>
#include <QImage>
#include <QLabel>
#include <QToolBar>
#include <QAction>
#include <QTextBrowser> // 用于侧拉日志框
#include <QDockWidget>  // 用于侧拉日志框
#include <QElapsedTimer>
#include <QTime>
#include <QVector>
#include <QSet>
#include <QBitArray>
#include <QMutex>
#include <QTimer>
#include <QThread>
#include <QSharedPointer>
#include <QAtomicInteger>
#include <QFile>
#include <QStackedWidget>

// 先引入组件头文件
#include "panoramawidget.h"
#include "aivideowidget.h"
#include "radarwidget.h"
#include "videothread.h" // 确保能识别 VideoThread 类型
#include "turntabledriver.h"
#include "turntablecontroldialog.h"
#include "panoramacache.h"
#include "panoramasaver.h"

namespace Ui { class MainWindow; }
class RawRecorder;
class CompactTargetRadarPanel;
class TargetRadarWindow;
#ifdef DMX_ADVANCED_DETECTION
class DirectYoloManager;
#endif

// Main-thread angle history used to place frames by receive-time lookup.
class AngleHistory
{
public:
    void add(qint64 tMs, double angleDeg) {
        m_t.push_back(tMs);
        m_a.push_back(normalize(angleDeg));
        const qint64 cutoff = tMs - 10000;
        int drop = 0;
        while (drop < m_t.size() && m_t[drop] < cutoff) ++drop;
        if (drop > 0) {
            m_t.remove(0, drop);
            m_a.remove(0, drop);
        }
    }

    void clear() {
        m_t.clear();
        m_a.clear();
    }

    bool empty() const { return m_t.isEmpty(); }
    int size() const { return m_t.size(); }

    int recentCount(qint64 nowMs, qint64 windowMs) const {
        int n = 0;
        for (int i = m_t.size() - 1; i >= 0; --i) {
            if ((nowMs - m_t[i]) > windowMs) break;
            ++n;
        }
        return n;
    }

    bool angleAt(qint64 tMs, double *outAngle, bool *outClamped, qint64 *outBracketMs) const {
        if (outClamped) *outClamped = false;
        if (outBracketMs) *outBracketMs = 0;
        const int n = m_t.size();
        if (n == 0 || !outAngle) return false;
        if (n == 1 || tMs <= m_t[0]) {
            *outAngle = m_a[0];
            if (outClamped) *outClamped = true;
            return true;
        }
        if (tMs >= m_t[n - 1]) {
            *outAngle = m_a[n - 1];
            if (outClamped) *outClamped = true;
            return true;
        }

        int hi = n - 1;
        while (hi > 0 && m_t[hi - 1] > tMs) --hi;
        const qint64 t0 = m_t[hi - 1];
        const qint64 t1 = m_t[hi];
        const double a0 = m_a[hi - 1];
        const double a1 = m_a[hi];
        if (outBracketMs) *outBracketMs = t1 - t0;
        if (t1 == t0) {
            *outAngle = a1;
            return true;
        }

        const double f = double(tMs - t0) / double(t1 - t0);
        double d = a1 - a0;
        while (d > 180.0) d -= 360.0;
        while (d < -180.0) d += 360.0;
        *outAngle = normalize(a0 + f * d);
        return true;
    }

private:
    static double normalize(double a) {
        while (a < 0.0) a += 360.0;
        while (a >= 360.0) a -= 360.0;
        return a;
    }

    QVector<qint64> m_t;
    QVector<double> m_a;
};

class RoiWorker : public QObject
{
    Q_OBJECT
public:
    explicit RoiWorker(QSharedPointer<PanoramaCache> cache, QObject *parent = nullptr);

public slots:
    void requestPreview(double angle);
    void requestFullRgbScaled(double angle, double scale);
    void requestFullBwScaled(double angle, double scale);

signals:
    void previewReady(double angle, const QImage &rgb, const QImage &bwRgb32);
    void fullScaledReady(bool isBw, double angle, double scale, const QImage &imgRgb32);

private slots:
    void process();

private:
    void schedule();

    QSharedPointer<PanoramaCache> m_cache;
    QMutex m_mtx;
    QAtomicInteger<int> m_scheduled = 0;

    bool m_pendingPreview = false;
    bool m_pendingFullRgb = false;
    bool m_pendingFullBw = false;
    double m_previewAngle = 0.0;
    double m_fullRgbAngle = 0.0;
    double m_fullBwAngle = 0.0;
    double m_fullRgbScale = 1.0;
    double m_fullBwScale = 1.0;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onActionOpenDevice();
    void onActionCloseDevice();
    void onActionSavePng();
    void onActionSaveJpg();
    void onActionSaveVideo();
    void onActionStopCapture();
    void onClearUiClicked();
    void onSaveFullPanoramaClicked();
    void onSaveFullPanoramaFinished(quint64 saveId, bool ok, const QString &msg, const QString &outDir);
    void onToggleRecording();
    void onOpenTargetRadarWindow();

    void onCommandReplyReceived();
    void onPathReceived(const QString &type, const QString &path, const QString &sender, qint64 rxMs);
    void onCandidateDetected(const QString &stream, double angle, int panoX, int panoY, double score, const QString &cropPath,
                             int roiBoxX1, int roiBoxY1, int roiBoxX2, int roiBoxY2,
                             const QString &className);
    void onStaticClutterDetected(const QString &stream,
                                 const QString &className,
                                 int panoX,
                                 int panoY,
                                 int trackId,
                                 int stableHits,
                                 double contextScore,
                                 const QString &cropPath);

    void onPanoramaClicked(double angle);
    void onRadarClicked(int angle);
    void onRenderTick();
    void drainRender();
    void onPanoRefreshTick();
    void onThumbRoiReady(double angle, const QImage &rgb, const QImage &bwRgb32);

    // 【新增】：日志写入槽函数
    void addLog(const QString &type, const QString &msg, const QString &color);

signals:
    void savePanoramaRequested(const QString &outDir, int previewJpegQuality, int previewMaxWidth);

private:
    void createToolBar();
    void updateUiState();
    void setupLogDock(); // 初始化日志界面
    double toRelativeAngle(double rawAngleDeg);
    void requestRecordingStartAtNextZero();
    void startRecordingNowAfterZero();
    void stopRecordingNow();
    void updateRecordActionText();
    static bool crossedZero(double prevAngleDeg, double currentAngleDeg);
    void updateReplaySweep(double angleDeg);
    void advanceReplaySweep();

    void sendCommand(const QString &cmd);
    void checkTargetDetection(double currentAngle);
    void initSimulatedTargets();
    void pruneDetectedRadarTargets(qint64 nowMs, bool removeScanned);
    bool scanCrossedAngle(double prevAngleDeg, double currentAngleDeg, double targetAngleDeg) const;
    void updateTargetRadarBackground();
    void replaceCandidateCropFromPanorama(const QString &stream,
                                          int panoX,
                                          int panoY,
                                          const QString &cropPath);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    Ui::MainWindow *ui;
    QStackedWidget *m_pageStack = nullptr;
    QWidget *m_legacyPage = nullptr;

    // UI 组件
    PanoramaWidget *panoramaView;
    PanoramaWidget *thermalPanoramaView;
    AIVideoWidget *colorRoiView;
    AIVideoWidget *thermalRoiView;
    CompactTargetRadarPanel *m_compactTargetRadar = nullptr;
    RadarWidget *radarView;
    AIVideoWidget *radarFeedbackView;

    QToolBar *m_mainToolBar;
    QTextBrowser *m_logBrowser;

    QLabel *m_angleLabel;
    QLabel *m_lapTimeLabel;

    // 网络与线程
    QUdpSocket *m_cmdSocket;
    QUdpSocket *m_replySocket;
    VideoThread *m_pathThread;
    VideoThread *m_colorThread;
    VideoThread *m_thermalThread;
#ifdef DMX_ADVANCED_DETECTION
    DirectYoloManager *m_directYoloManager = nullptr;
#endif

    TurntableDriver *m_driver;
    TurntableControlDialog *m_ctrlDialog;

    // 全局数据（已修复重复定义）
    QSharedPointer<PanoramaCache> m_panoCache;
    QAtomicInteger<int> m_renderPending;
    QImage m_uiThumbRgb;
    QImage m_uiThumbBw;
    // Panorama display is driven by a steady ~30fps timer doing a full redraw
    // (decoupled from frame arrival) to avoid the tile-by-tile blocky refresh.
    QTimer *m_panoRefreshTimer = nullptr;
    bool m_panoRgbDirty = false;
    bool m_panoBwDirty = false;

    bool m_isDeviceOpen;
    bool m_replayMode = false;
    QTimer *m_replaySweepTimer = nullptr;
    QElapsedTimer m_replaySweepClock;
    double m_replaySweepAngleDeg = 0.0;
    bool m_replayMappingLogged = false;
    bool m_waitingForRunAngle = false;
    double m_latestAngle;
    double m_prevCheckAngle;
    double m_prevRadarSweepAngle = -1.0;
    QVector<RadarTarget> m_simTargets;
    QVector<qint64> m_detectTargetMs;
    qint64 m_lastDetectAutoSwitchMs = 0;
    bool m_zeroAngleInited = false;
    double m_zeroAngleRaw = 0.0;
    AngleHistory m_angleHistory;
    qint64 m_captureLatencyMs = 0;
    qint64 m_captureLatencyRgbMs = 0;
    qint64 m_captureLatencyBwMs = 0;
    bool m_angleLookup = true;
    qint64 m_lastAngleDiagMs = 0;
    QImage m_lastColorRoi;
    QImage m_lastThermalRoi;
    double m_lastRoiAngle = -1.0;
    double m_pendingPopupRgbAngle = -1.0;
    double m_pendingPopupBwAngle = -1.0;

    QElapsedTimer m_perfTimer;
    qint64 m_lastDetectMs;
    qint64 m_lastLogMs;

    // 动作项
    QAction *m_actOpenDevice;
    QAction *m_actCloseDevice;
    QAction *m_actSavePng;
    QAction *m_actSaveJpg;
    QAction *m_actSaveVideo;
    QAction *m_actStopCapture;
    QAction *m_actClearImage;
    QAction *m_actSaveFullPanorama;
    QAction *m_actRecord;
    QAction *m_actTargetRadar;
    QAction *m_actExit;

    QThread *m_saveThread = nullptr;
    PanoramaSaver *m_saveWorker = nullptr;

    QThread *m_roiThread = nullptr;
    RoiWorker *m_roiWorker = nullptr;

    QThread *m_recordThread = nullptr;
    RawRecorder *m_recordWorker = nullptr;
    bool m_isRecording = false;
    bool m_recordPendingZero = false;
    bool m_recordPendingHasAngle = false;
    double m_recordPendingPrevAngle = 0.0;

    QFile *m_logFile = nullptr;
    TargetRadarWindow *m_targetRadarWindow = nullptr;
    quint64 m_targetRadarSeq = 0;
    qint64 m_lastTargetRadarBgMs = 0;
};

#endif // MAINWINDOW_H
