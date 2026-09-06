#ifndef TURNTABLECONTROLDIALOG_H
#define TURNTABLECONTROLDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QString>

// 引入我们刚刚剥离出来的底层纯净驱动
#include "turntabledriver.h"

class QShowEvent;

class TurntableControlDialog : public QDialog
{
    Q_OBJECT

public:
    struct Settings {
        QString serialPort;
        int baudRate;
        QString direction;
        int speed;
        bool orthoEnabled;
        int orthoLength;
        bool feedbackEnabled;
    };

    // 构造函数：必须接收一个由主窗口(MainWindow)创建好的 driver 指针
    explicit TurntableControlDialog(TurntableDriver *driver, QWidget *parent = nullptr);
    ~TurntableControlDialog() = default;

    void applySettings(const Settings &settings);
    Settings currentSettings() const;
    Settings startupSettings() const;
    bool runWithCurrentSettings(QString *errMsg);
    bool runWithStartupSettings(Settings *appliedSettings, QString *errMsg);
    void stopAndDisableOrtho();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void refreshSerialPorts(bool keepCurrent);
    int nextCommandSequence();
    bool runWithSettings(const Settings &settings, QString *errMsg);

    // ================= 核心驱动指针 =================
    // 注意：Dialog 只“使用”这个驱动，不负责“销毁”它。它的生命周期归 MainWindow 管
    TurntableDriver *m_driver;
    int m_commandSequence = 0;
    Settings m_startupSettings;

    // ================= 界面控件声明 =================
    // 1. 串口配置区
    QComboBox *cmbPorts;
    QComboBox *cmbBaudRate;
    QPushButton *btnOpenSerial;

    // 2. 基础运动控制区 (已将微调框修改为精准档位下拉框)
    QComboBox *cmbSpeed;
    QComboBox *cmbDirection;
    QPushButton *btnLeft;
    QPushButton *btnRight;
    QPushButton *btnStop;

    // 3. 正交控制与实时回传区
    QPushButton *btnEnableOrtho;
    QPushButton *btnDisableOrtho;
    QComboBox *cmbOrthoLength;
    QCheckBox *chkOrthoEnabled;
    QPushButton *btnSetLength;

    QPushButton *btnEnableFeedback;
    QPushButton *btnDisableFeedback;
    QCheckBox *chkFeedbackEnabled;
};

#endif // TURNTABLECONTROLDIALOG_H
