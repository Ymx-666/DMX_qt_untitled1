#ifndef TURNTABLECONTROLDIALOG_H
#define TURNTABLECONTROLDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QPushButton>

// 引入我们刚刚剥离出来的底层纯净驱动
#include "turntabledriver.h"

class TurntableControlDialog : public QDialog
{
    Q_OBJECT

public:
    // 构造函数：必须接收一个由主窗口(MainWindow)创建好的 driver 指针
    explicit TurntableControlDialog(TurntableDriver *driver, QWidget *parent = nullptr);
    ~TurntableControlDialog() = default;

private:
    // ================= 核心驱动指针 =================
    // 注意：Dialog 只“使用”这个驱动，不负责“销毁”它。它的生命周期归 MainWindow 管
    TurntableDriver *m_driver;

    // ================= 界面控件声明 =================
    // 1. 串口配置区
    QComboBox *cmbPorts;
    QPushButton *btnOpenSerial;

    // 2. 基础运动控制区 (已将微调框修改为精准档位下拉框)
    QComboBox *cmbSpeed;
    QPushButton *btnLeft;
    QPushButton *btnRight;
    QPushButton *btnStop;

    // 3. 正交控制与实时回传区
    QPushButton *btnEnableOrtho;
    QPushButton *btnDisableOrtho;
    QComboBox *cmbOrthoLength;
    QPushButton *btnSetLength;

    QPushButton *btnEnableFeedback;
    QPushButton *btnDisableFeedback;
};

#endif // TURNTABLECONTROLDIALOG_H
