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

    void onCommandReplyReceived();
    void onPathReceived(const QString &type, const QString &path, const QString &sender, qint64 rxMs);

    void onPanoramaClicked(double angle);
    void onRadarClicked(int angle);
    void onRenderTick();
    void drainRender();
    void onThumbRoiReady(double angle, const QImage &rgb, const QImage &bwRgb32);

    // 【新增】：日志写入槽函数
    void addLog(const QString &type, const QString &msg, const QString &color);

signals:
    void savePanoramaRequested(const QString &outDir, int rgbJpegQuality);

private:
    void createToolBar();
    void updateUiState();
    void setupLogDock(); // 初始化日志界面
    double toRelativeAngle(double rawAngleDeg);

    void sendCommand(const QString &cmd);
    void checkTargetDetection(double currentAngle);
    void initSimulatedTargets();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    Ui::MainWindow *ui;

    // UI 组件
    PanoramaWidget *panoramaView;
    PanoramaWidget *thermalPanoramaView;
    AIVideoWidget *colorRoiView;
    AIVideoWidget *thermalRoiView;
    AIVideoWidget *captureView;
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

    TurntableDriver *m_driver;
    TurntableControlDialog *m_ctrlDialog;

    // 全局数据（已修复重复定义）
    QSharedPointer<PanoramaCache> m_panoCache;
    QAtomicInteger<int> m_renderPending;
    QImage m_uiThumbRgb;
    QImage m_uiThumbBw;

    bool m_isDeviceOpen;
    double m_latestAngle;
    double m_prevCheckAngle;
    QVector<RadarTarget> m_simTargets;
    bool m_zeroAngleInited = false;
    double m_zeroAngleRaw = 0.0;
    double m_pendingCaptureAngle = 0.0;
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
    QAction *m_actExit;

    QThread *m_saveThread = nullptr;
    PanoramaSaver *m_saveWorker = nullptr;

    QThread *m_roiThread = nullptr;
    RoiWorker *m_roiWorker = nullptr;

    QThread *m_recordThread = nullptr;
    RawRecorder *m_recordWorker = nullptr;
    bool m_isRecording = false;

    QFile *m_logFile = nullptr;
};

#endif // MAINWINDOW_H
