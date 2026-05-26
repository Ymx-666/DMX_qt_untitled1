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
    this->setWindowTitle("Turntable Control");
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
    btnOpenSerial = new QPushButton("Open COM", this);
    laySerial->addWidget(new QLabel("COM:", this));
    laySerial->addWidget(cmbPorts);
    laySerial->addWidget(btnOpenSerial);
    mainLayout->addLayout(laySerial);

    // ==========================================
    // 2. 基础运动区 (精准档位下拉框)
    // ==========================================
    QGridLayout *layMove = new QGridLayout();

    cmbSpeed = new QComboBox(this);
    // addItem("显示的文字", 隐藏的物理指令值);
    cmbSpeed->addItem("Fast (~1.32s/lap)", 255);
    cmbSpeed->addItem("2s / lap", 170);
    cmbSpeed->addItem("4s / lap", 85);
    cmbSpeed->addItem("6s / lap", 57);
    cmbSpeed->addItem("8s / lap", 43);
    // 默认选中 4s/圈 (索引2)
    cmbSpeed->setCurrentIndex(2);

    btnLeft = new QPushButton("Left", this);
    btnRight = new QPushButton("Right", this);
    btnStop = new QPushButton("STOP", this);
    btnStop->setStyleSheet("background-color: #ffcccc; font-weight: bold;");

    layMove->addWidget(new QLabel("Speed:"), 0, 0);
    layMove->addWidget(cmbSpeed, 0, 1, 1, 2);
    layMove->addWidget(btnLeft, 1, 0);
    layMove->addWidget(btnStop, 1, 1);
    layMove->addWidget(btnRight, 1, 2);
    mainLayout->addLayout(layMove);

    // ==========================================
    // 3. 正交与回传区
    // ==========================================
    QGridLayout *layAdvanced = new QGridLayout();
    btnEnableOrtho = new QPushButton("Ortho ON", this);
    btnDisableOrtho = new QPushButton("Ortho OFF", this);
    cmbOrthoLength = new QComboBox(this);
    cmbOrthoLength->addItems({"256", "512", "1024", "2048", "4096"});
    btnSetLength = new QPushButton("Set Pack Len", this);

    btnEnableFeedback = new QPushButton("Feedback ON", this);
    btnEnableFeedback->setStyleSheet("color: green;");
    btnDisableFeedback = new QPushButton("Feedback OFF", this);
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
            btnOpenSerial->setText("Open COM");
        } else {
            if (m_driver->openPort(cmbPorts->currentText())) {
                btnOpenSerial->setText("Close COM");
            } else {
                QMessageBox::warning(this, "Error", "Open COM failed");
            }
        }
        btnOpenSerial->setEnabled(true);
    });

    // speed control
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
