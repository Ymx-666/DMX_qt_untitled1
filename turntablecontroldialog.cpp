#include "turntablecontroldialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QCoreApplication>

// 构造函数中传入之前实例化好的 driver
TurntableControlDialog::TurntableControlDialog(TurntableDriver *driver, QWidget *parent)
    : QDialog(parent), m_driver(driver)
{
    this->setWindowTitle("云台快速控制中心");
    this->resize(350, 400);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // ==========================================
    // 1. 串口控制区
    // ==========================================
    QHBoxLayout *laySerial = new QHBoxLayout();
    cmbPorts = new QComboBox(this);
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        cmbPorts->addItem(info.portName());
    }
    btnOpenSerial = new QPushButton("打开串口", this);
    laySerial->addWidget(new QLabel("串口:", this));
    laySerial->addWidget(cmbPorts);
    laySerial->addWidget(btnOpenSerial);
    mainLayout->addLayout(laySerial);

    // ==========================================
    // 2. 基础运动区 (精准档位下拉框)
    // ==========================================
    QGridLayout *layMove = new QGridLayout();

    cmbSpeed = new QComboBox(this);
    // addItem("显示的文字", 隐藏的物理指令值);
    cmbSpeed->addItem("极速 (约1.32s/圈)", 255);
    cmbSpeed->addItem("2s / 圈", 170);
    cmbSpeed->addItem("4s / 圈", 85);
    cmbSpeed->addItem("6s / 圈", 57);
    cmbSpeed->addItem("8s / 圈", 43);
    // 默认选中 4s/圈 (索引2)
    cmbSpeed->setCurrentIndex(2);

    btnLeft = new QPushButton("◀ 向左转", this);
    btnRight = new QPushButton("向右转 ▶", this);
    btnStop = new QPushButton("急停", this);
    btnStop->setStyleSheet("background-color: #ffcccc; font-weight: bold;");

    layMove->addWidget(new QLabel("运行速度:"), 0, 0);
    layMove->addWidget(cmbSpeed, 0, 1, 1, 2);
    layMove->addWidget(btnLeft, 1, 0);
    layMove->addWidget(btnStop, 1, 1);
    layMove->addWidget(btnRight, 1, 2);
    mainLayout->addLayout(layMove);

    // ==========================================
    // 3. 正交与回传区
    // ==========================================
    QGridLayout *layAdvanced = new QGridLayout();
    btnEnableOrtho = new QPushButton("开正交", this);
    btnDisableOrtho = new QPushButton("关正交", this);
    cmbOrthoLength = new QComboBox(this);
    cmbOrthoLength->addItems({"256", "512", "1024", "2048", "4096"});
    btnSetLength = new QPushButton("设置打包长度", this);

    btnEnableFeedback = new QPushButton("开启实时回传", this);
    btnEnableFeedback->setStyleSheet("color: green;");
    btnDisableFeedback = new QPushButton("关闭实时回传", this);
    btnDisableFeedback->setStyleSheet("color: red;");

    layAdvanced->addWidget(btnEnableOrtho, 0, 0);
    layAdvanced->addWidget(btnDisableOrtho, 0, 1);
    layAdvanced->addWidget(cmbOrthoLength, 1, 0);
    layAdvanced->addWidget(btnSetLength, 1, 1);
    layAdvanced->addWidget(btnEnableFeedback, 2, 0);
    layAdvanced->addWidget(btnDisableFeedback, 2, 1);
    mainLayout->addLayout(layAdvanced);

    // ==========================================
    // ====== 信号绑定 ======
    // ==========================================
    connect(btnOpenSerial, &QPushButton::clicked, this, [=]() {
        btnOpenSerial->setEnabled(false);
        QCoreApplication::processEvents();
        if (m_driver->isOpen()) {
            m_driver->closePort();
            btnOpenSerial->setText("打开串口");
        } else {
            if (m_driver->openPort(cmbPorts->currentText())) {
                btnOpenSerial->setText("关闭串口");
            } else {
                QMessageBox::warning(this, "错误", "串口打开失败！");
            }
        }
        btnOpenSerial->setEnabled(true);
    });

    // 1. 点击“向左转/向右转”，下发运动指令，读取下拉框隐藏的 currentData 值
    connect(btnLeft, &QPushButton::clicked, this, [=]() {
        m_driver->turnLeft(cmbSpeed->currentData().toInt());
    });
    connect(btnRight, &QPushButton::clicked, this, [=]() {
        m_driver->turnRight(cmbSpeed->currentData().toInt());
    });

    // 2. 急停指令
    connect(btnStop, &QPushButton::clicked, this, [=]() { m_driver->stop(); });

    // 3. 高级控制区信号绑定
    connect(btnEnableOrtho, &QPushButton::clicked, this, [=]() { m_driver->enableOrtho(); });
    connect(btnDisableOrtho, &QPushButton::clicked, this, [=]() { m_driver->disableOrtho(); });
    connect(btnSetLength, &QPushButton::clicked, this, [=]() { m_driver->setOrthoLength(cmbOrthoLength->currentText().toInt()); });
    connect(btnEnableFeedback, &QPushButton::clicked, this, [=]() { m_driver->enableFeedback(); });
    connect(btnDisableFeedback, &QPushButton::clicked, this, [=]() { m_driver->disableFeedback(); });
}
