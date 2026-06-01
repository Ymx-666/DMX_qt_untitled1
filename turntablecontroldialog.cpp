#include "turntablecontroldialog.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QShowEvent>
#include <QThread>
#include <QVariant>
#include <QVBoxLayout>

static void setComboByData(QComboBox *combo, const QVariant &value)
{
    if (!combo) return;
    const int idx = combo->findData(value);
    if (idx >= 0) combo->setCurrentIndex(idx);
}

static void setComboByText(QComboBox *combo, const QString &text)
{
    if (!combo) return;
    const int idx = combo->findText(text);
    if (idx >= 0) combo->setCurrentIndex(idx);
}

TurntableControlDialog::TurntableControlDialog(TurntableDriver *driver, QWidget *parent)
    : QDialog(parent), m_driver(driver)
{
    this->setWindowTitle("Turntable Control");
    this->resize(420, 430);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QHBoxLayout *laySerial = new QHBoxLayout();
    cmbPorts = new QComboBox(this);
    refreshSerialPorts(false);
    cmbBaudRate = new QComboBox(this);
    cmbBaudRate->addItem("9600", 9600);
    cmbBaudRate->addItem("19200", 19200);
    cmbBaudRate->addItem("38400", 38400);
    cmbBaudRate->addItem("57600", 57600);
    cmbBaudRate->addItem("115200", 115200);
    btnOpenSerial = new QPushButton("Open COM", this);
    laySerial->addWidget(new QLabel("COM:", this));
    laySerial->addWidget(cmbPorts);
    laySerial->addWidget(new QLabel("Baud:", this));
    laySerial->addWidget(cmbBaudRate);
    laySerial->addWidget(btnOpenSerial);
    mainLayout->addLayout(laySerial);

    QGridLayout *layMove = new QGridLayout();
    cmbDirection = new QComboBox(this);
    cmbDirection->addItem("Right", "right");
    cmbDirection->addItem("Left", "left");

    cmbSpeed = new QComboBox(this);
    cmbSpeed->addItem("Fast (~1.32s/lap)", 255);
    cmbSpeed->addItem("2s / lap", 170);
    cmbSpeed->addItem("4s / lap", 85);
    cmbSpeed->addItem("6s / lap", 57);
    cmbSpeed->addItem("8s / lap", 43);
    cmbSpeed->setCurrentIndex(2);

    btnLeft = new QPushButton("Left", this);
    btnRight = new QPushButton("Right", this);
    btnStop = new QPushButton("STOP", this);
    btnStop->setStyleSheet("background-color: #ffcccc; font-weight: bold;");

    layMove->addWidget(new QLabel("Direction:"), 0, 0);
    layMove->addWidget(cmbDirection, 0, 1, 1, 2);
    layMove->addWidget(new QLabel("Speed:"), 1, 0);
    layMove->addWidget(cmbSpeed, 1, 1, 1, 2);
    layMove->addWidget(btnLeft, 2, 0);
    layMove->addWidget(btnStop, 2, 1);
    layMove->addWidget(btnRight, 2, 2);
    mainLayout->addLayout(layMove);

    QGridLayout *layAdvanced = new QGridLayout();
    btnEnableOrtho = new QPushButton("Ortho ON", this);
    btnDisableOrtho = new QPushButton("Ortho OFF", this);
    cmbOrthoLength = new QComboBox(this);
    cmbOrthoLength->addItems({"256", "512", "1024", "2048", "4096"});
    chkOrthoEnabled = new QCheckBox("Use Ortho", this);
    btnSetLength = new QPushButton("Set Pack Len", this);

    btnEnableFeedback = new QPushButton("Feedback ON", this);
    btnEnableFeedback->setStyleSheet("color: green;");
    btnDisableFeedback = new QPushButton("Feedback OFF", this);
    btnDisableFeedback->setStyleSheet("color: red;");
    chkFeedbackEnabled = new QCheckBox("Use Feedback", this);

    layAdvanced->addWidget(btnEnableOrtho, 0, 0);
    layAdvanced->addWidget(btnDisableOrtho, 0, 1);
    layAdvanced->addWidget(chkOrthoEnabled, 1, 0);
    layAdvanced->addWidget(cmbOrthoLength, 1, 1);
    layAdvanced->addWidget(btnSetLength, 2, 0, 1, 2);
    layAdvanced->addWidget(btnEnableFeedback, 3, 0);
    layAdvanced->addWidget(btnDisableFeedback, 3, 1);
    layAdvanced->addWidget(chkFeedbackEnabled, 4, 0, 1, 2);
    mainLayout->addLayout(layAdvanced);

    connect(btnOpenSerial, &QPushButton::clicked, this, [=]() {
        btnOpenSerial->setEnabled(false);
        QCoreApplication::processEvents();
        if (m_driver->isOpen()) {
            m_driver->closePort();
            btnOpenSerial->setText("Open COM");
        } else {
            if (m_driver->openPort(cmbPorts->currentText(), cmbBaudRate->currentData().toInt())) {
                btnOpenSerial->setText("Close COM");
            } else {
                QMessageBox::warning(this, "Error", "Open COM failed");
            }
        }
        btnOpenSerial->setEnabled(true);
    });

    connect(btnLeft, &QPushButton::clicked, this, [=]() {
        setComboByData(cmbDirection, QStringLiteral("left"));
        m_driver->turnLeft(cmbSpeed->currentData().toInt());
    });
    connect(btnRight, &QPushButton::clicked, this, [=]() {
        setComboByData(cmbDirection, QStringLiteral("right"));
        m_driver->turnRight(cmbSpeed->currentData().toInt());
    });
    connect(btnStop, &QPushButton::clicked, this, [=]() { m_driver->stop(); });

    connect(btnEnableOrtho, &QPushButton::clicked, this, [=]() {
        chkOrthoEnabled->setChecked(true);
        m_driver->enableOrtho();
    });
    connect(btnDisableOrtho, &QPushButton::clicked, this, [=]() {
        chkOrthoEnabled->setChecked(false);
        m_driver->disableOrtho();
    });
    connect(btnSetLength, &QPushButton::clicked, this, [=]() {
        m_driver->setOrthoLength(cmbOrthoLength->currentText().toInt());
    });
    connect(btnEnableFeedback, &QPushButton::clicked, this, [=]() {
        chkFeedbackEnabled->setChecked(true);
        m_driver->enableFeedback();
    });
    connect(btnDisableFeedback, &QPushButton::clicked, this, [=]() {
        chkFeedbackEnabled->setChecked(false);
        m_driver->disableFeedback();
    });
}

void TurntableControlDialog::applySettings(const TurntableControlDialog::Settings &settings)
{
    if (!settings.serialPort.trimmed().isEmpty()) {
        const QString port = settings.serialPort.trimmed();
        if (cmbPorts->findText(port) < 0) cmbPorts->addItem(port);
        setComboByText(cmbPorts, port);
    }
    if (cmbBaudRate->findData(settings.baudRate) < 0) {
        cmbBaudRate->addItem(QString::number(settings.baudRate), settings.baudRate);
    }
    setComboByData(cmbBaudRate, settings.baudRate);
    setComboByData(cmbDirection, settings.direction.trimmed().toLower());
    if (cmbSpeed->findData(settings.speed) < 0) {
        cmbSpeed->addItem(QString::number(settings.speed), settings.speed);
    }
    setComboByData(cmbSpeed, settings.speed);
    if (cmbOrthoLength->findText(QString::number(settings.orthoLength)) < 0) {
        cmbOrthoLength->addItem(QString::number(settings.orthoLength));
    }
    setComboByText(cmbOrthoLength, QString::number(settings.orthoLength));
    chkOrthoEnabled->setChecked(settings.orthoEnabled);
    chkFeedbackEnabled->setChecked(settings.feedbackEnabled);
}

void TurntableControlDialog::showEvent(QShowEvent *event)
{
    refreshSerialPorts(true);
    QDialog::showEvent(event);
}

void TurntableControlDialog::refreshSerialPorts(bool keepCurrent)
{
    const QString current = keepCurrent ? cmbPorts->currentText().trimmed() : QString();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        const QString port = info.portName().trimmed();
        if (!port.isEmpty() && cmbPorts->findText(port) < 0) {
            cmbPorts->addItem(port);
        }
    }
    if (!current.isEmpty()) {
        setComboByText(cmbPorts, current);
    } else if (cmbPorts->currentText().trimmed().isEmpty() && cmbPorts->count() > 0) {
        cmbPorts->setCurrentIndex(0);
    }
}

TurntableControlDialog::Settings TurntableControlDialog::currentSettings() const
{
    Settings s;
    s.serialPort = cmbPorts->currentText().trimmed();
    s.baudRate = cmbBaudRate->currentData().toInt();
    s.direction = cmbDirection->currentData().toString().trimmed().toLower();
    if (s.direction != QStringLiteral("left")) s.direction = QStringLiteral("right");
    s.speed = cmbSpeed->currentData().toInt();
    s.orthoEnabled = chkOrthoEnabled->isChecked();
    s.orthoLength = cmbOrthoLength->currentText().toInt();
    s.feedbackEnabled = chkFeedbackEnabled->isChecked();
    return s;
}

bool TurntableControlDialog::runWithCurrentSettings(QString *errMsg)
{
    if (!m_driver) {
        if (errMsg) *errMsg = QStringLiteral("Turntable driver is not ready");
        return false;
    }

    refreshSerialPorts(true);
    const Settings s = currentSettings();
    if (!m_driver->isOpen()) {
        if (s.serialPort.isEmpty()) {
            if (errMsg) *errMsg = QStringLiteral("No serial port selected");
            return false;
        }
        if (!m_driver->openPort(s.serialPort, s.baudRate)) {
            if (errMsg) *errMsg = QStringLiteral("Open COM failed: %1").arg(s.serialPort);
            return false;
        }
        btnOpenSerial->setText("Close COM");
        QCoreApplication::processEvents();
        QThread::msleep(120);
    }

    if (s.orthoEnabled) m_driver->enableOrtho();
    else m_driver->disableOrtho();
    QThread::msleep(40);
    m_driver->setOrthoLength(s.orthoLength);
    QThread::msleep(40);
    if (s.feedbackEnabled) m_driver->enableFeedback();
    else m_driver->disableFeedback();
    QThread::msleep(40);

    if (s.direction == QStringLiteral("left")) m_driver->turnLeft(s.speed);
    else m_driver->turnRight(s.speed);
    return true;
}
