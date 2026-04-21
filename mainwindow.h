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
#include <QTime>
#include <QVector>

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

    void onCommandReplyReceived();
    void onColorFrameReceived(QImage img, int fileIndex);
    void onThermalFrameReceived(QImage img, int fileIndex);

    void onPanoramaClicked(double angle);
    void onRadarClicked(int angle);

    // 【新增】：日志写入槽函数
    void addLog(const QString &type, const QString &msg, const QString &color);

private:
    void createToolBar();
    void updateUiState();
    void setupLogDock(); // 初始化日志界面

    void sendCommand(const QString &cmd);
    void updatePanoramaSlice(const QImage &frame, int fileIndex, int type);
    QImage fetchRoiFromPageTable(int file_idx, int type);
    void checkTargetDetection(double currentAngle);
    void initSimulatedTargets();

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
    VideoThread *m_colorThread;
    VideoThread *m_thermalThread;

    TurntableDriver *m_driver;
    TurntableControlDialog *m_ctrlDialog;

    // 全局数据（已修复重复定义）
    QImage fullPanoramaImage;
    QImage fullThermalPanoramaImage;
    QImage m_highResColorPanorama;
    QImage m_highResThermalPanorama;

    bool m_isDeviceOpen;
    double m_latestAngle;
    double m_prevCheckAngle;
    QStringList m_imagePaths;
    QVector<RadarTarget> m_simTargets;

    // 动作项
    QAction *m_actOpenDevice;
    QAction *m_actCloseDevice;
    QAction *m_actSavePng;
    QAction *m_actSaveJpg;
    QAction *m_actSaveVideo;
    QAction *m_actStopCapture;
    QAction *m_actClearImage;
    QAction *m_actExit;
};

#endif // MAINWINDOW_H
