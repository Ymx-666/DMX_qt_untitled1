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

// 先引入组件头文件
#include "panoramawidget.h"
#include "aivideowidget.h"
#include "radarwidget.h"
#include "videothread.h" // 确保能识别 VideoThread 类型
#include "turntabledriver.h"
#include "turntablecontroldialog.h"

namespace Ui { class MainWindow; }

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
    void onSaveFullPanoramaFinished(bool ok, const QString &msg, const QString &rgbPath, const QString &bwPath);

    void onCommandReplyReceived();
    void onColorFrameReceived(QImage img, double angleDeg);
    void onThermalFrameReceived(QImage img, double angleDeg);
    void onPathReceived(const QString &type, const QString &path, const QString &sender);
    void onColorRoiCaptured(QImage img, int tag);
    void onThermalRoiCaptured(QImage img, int tag);
    void onRgbPanoramaSnapshotReady(QImage img);
    void onBwPanoramaSnapshotReady(QImage img);

    void onPanoramaClicked(double angle);
    void onRadarClicked(int angle);

    // 【新增】：日志写入槽函数
    void addLog(const QString &type, const QString &msg, const QString &color);

private:
    void createToolBar();
    void updateUiState();
    void setupLogDock(); // 初始化日志界面
    double toRelativeAngle(double rawAngleDeg);

    void sendCommand(const QString &cmd);
    void updatePanoramaSliceByAngle(const QImage &frame, double angleDeg, int type);
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
    QImage fullPanoramaImage;
    QImage fullThermalPanoramaImage;

    bool m_isDeviceOpen;
    double m_latestAngle;
    double m_prevCheckAngle;
    QVector<RadarTarget> m_simTargets;
    bool m_zeroAngleInited = false;
    double m_zeroAngleRaw = 0.0;
    double m_pendingCaptureAngle = 0.0;
    QImage m_lastColorRoi;
    QImage m_lastThermalRoi;
    QImage m_pendingSaveRgb;
    QImage m_pendingSaveBw;
    bool m_pendingRadarFeedback = false;
    QString m_pendingSaveRgbPath;
    QString m_pendingSaveBwPath;

    QElapsedTimer m_perfTimer;
    qint64 m_lastColorUiMs;
    qint64 m_lastThermalUiMs;
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
    QAction *m_actExit;

    QMutex m_fullSaveMutex;
    QBitArray m_rgbSegFilled;
    QBitArray m_bwSegFilled;
    int m_rgbSegments;
    int m_bwSegments;
    bool m_isSavingFullPanorama;
    int m_pendingSaveSnapshots = 0;
};

#endif // MAINWINDOW_H
