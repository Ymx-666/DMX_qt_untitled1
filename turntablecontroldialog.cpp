#include "turntablecontroldialog.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QShowEvent>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <functional>

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

static QString zh(const char *text)
{
    return QString::fromUtf8(text);
}

TurntableControlDialog::TurntableControlDialog(TurntableDriver *driver, QWidget *parent)
    : QDialog(parent), m_driver(driver)
{
    this->setWindowTitle(zh("转台控制"));
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
    btnOpenSerial = new QPushButton(zh("打开串口"), this);
    laySerial->addWidget(new QLabel(zh("串口:"), this));
    laySerial->addWidget(cmbPorts);
    laySerial->addWidget(new QLabel(zh("波特率:"), this));
    laySerial->addWidget(cmbBaudRate);
    laySerial->addWidget(btnOpenSerial);
    mainLayout->addLayout(laySerial);

    QGridLayout *layMove = new QGridLayout();
    cmbDirection = new QComboBox(this);
    cmbDirection->addItem(zh("左转"), "left");
    cmbDirection->addItem(zh("右转"), "right");

    cmbSpeed = new QComboBox(this);
    cmbSpeed->addItem(zh("最快（约1.32秒/圈）"), 255);
    cmbSpeed->addItem(zh("2秒/圈"), 170);
    cmbSpeed->addItem(zh("4秒/圈"), 85);
    cmbSpeed->addItem(zh("6秒/圈"), 57);
    cmbSpeed->addItem(zh("8秒/圈"), 43);
    cmbSpeed->setObjectName(QStringLiteral("turntableSpeedCombo"));
    setComboByData(cmbSpeed, 43);

    btnLeft = new QPushButton(zh("左转"), this);
    btnRight = new QPushButton(zh("右转"), this);
    btnStop = new QPushButton(zh("停止"), this);
    btnStop->setStyleSheet("background-color: #ffcccc; font-weight: bold;");

    layMove->addWidget(new QLabel(zh("方向:"), this), 0, 0);
    layMove->addWidget(cmbDirection, 0, 1, 1, 2);
    layMove->addWidget(new QLabel(zh("速度:"), this), 1, 0);
    layMove->addWidget(cmbSpeed, 1, 1, 1, 2);
    layMove->addWidget(btnLeft, 2, 0);
    layMove->addWidget(btnStop, 2, 1);
    layMove->addWidget(btnRight, 2, 2);
    mainLayout->addLayout(layMove);

    QGridLayout *layAdvanced = new QGridLayout();
    btnEnableOrtho = new QPushButton(zh("开启正交输出"), this);
    btnDisableOrtho = new QPushButton(zh("关闭正交输出"), this);
    cmbOrthoLength = new QComboBox(this);
    cmbOrthoLength->addItems({"256", "512", "1024", "2048", "4096"});
    chkOrthoEnabled = new QCheckBox(zh("正交输出已开启"), this);
    chkOrthoEnabled->setChecked(false);
    chkOrthoEnabled->setEnabled(false);
    btnSetLength = new QPushButton(zh("设置正交包长"), this);

    btnEnableFeedback = new QPushButton(zh("开启角度回传"), this);
    btnEnableFeedback->setStyleSheet("color: green;");
    btnDisableFeedback = new QPushButton(zh("关闭角度回传"), this);
    btnDisableFeedback->setStyleSheet("color: red;");
    chkFeedbackEnabled = new QCheckBox(zh("启用角度回传"), this);

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
            btnOpenSerial->setText(zh("打开串口"));
        } else {
            if (m_driver->openPort(cmbPorts->currentText(), cmbBaudRate->currentData().toInt())) {
                btnOpenSerial->setText(zh("关闭串口"));
            } else {
                QMessageBox::warning(this, zh("错误"), zh("打开串口失败"));
            }
        }
        btnOpenSerial->setEnabled(true);
    });

    connect(btnLeft, &QPushButton::clicked, this, [=]() {
        setComboByData(cmbDirection, QStringLiteral("left"));
        QString err;
        if (!runWithCurrentSettings(&err)) QMessageBox::warning(this, zh("\xE9\x94\x99\xE8\xAF\xAF"), err);
    });
    connect(btnRight, &QPushButton::clicked, this, [=]() {
        setComboByData(cmbDirection, QStringLiteral("right"));
        QString err;
        if (!runWithCurrentSettings(&err)) QMessageBox::warning(this, zh("\xE9\x94\x99\xE8\xAF\xAF"), err);
    });
    connect(btnStop, &QPushButton::clicked, this, [=]() { stopAndDisableOrtho(); });

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

    m_startupSettings = currentSettings();
}

void TurntableControlDialog::applySettings(const TurntableControlDialog::Settings &settings)
{
    m_startupSettings = settings;
    m_startupSettings.serialPort = settings.serialPort.trimmed();
    m_startupSettings.direction = settings.direction.trimmed().toLower();
    if (m_startupSettings.direction != QStringLiteral("left")) {
        m_startupSettings.direction = QStringLiteral("right");
    }
    m_startupSettings.speed = qBound(1, settings.speed, 255);

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

int TurntableControlDialog::nextCommandSequence()
{
    return ++m_commandSequence;
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

TurntableControlDialog::Settings TurntableControlDialog::startupSettings() const
{
    Settings s = m_startupSettings;
    if (s.serialPort.isEmpty()) {
        s.serialPort = currentSettings().serialPort;
    }
    return s;
}

bool TurntableControlDialog::runWithCurrentSettings(QString *errMsg)
{
    refreshSerialPorts(true);
    return runWithSettings(currentSettings(), errMsg);
}

bool TurntableControlDialog::runWithStartupSettings(Settings *appliedSettings, QString *errMsg)
{
    refreshSerialPorts(true);
    const Settings s = startupSettings();
    if (appliedSettings) *appliedSettings = s;
    return runWithSettings(s, errMsg);
}

bool TurntableControlDialog::runWithSettings(const Settings &s, QString *errMsg)
{
    if (!m_driver) {
        if (errMsg) *errMsg = zh("转台驱动未就绪");
        return false;
    }

    if (!m_driver->isOpen()) {
        if (s.serialPort.isEmpty()) {
            if (errMsg) *errMsg = zh("未选择串口");
            return false;
        }
        if (!m_driver->openPort(s.serialPort, s.baudRate)) {
            if (errMsg) *errMsg = zh("打开串口失败: %1").arg(s.serialPort);
            return false;
        }
        btnOpenSerial->setText(zh("关闭串口"));
    }

    const int seq = nextCommandSequence();
    chkOrthoEnabled->setChecked(false);

    auto schedule = [this, seq](int delayMs, const std::function<void()> &fn) {
        QTimer::singleShot(delayMs, this, [this, seq, fn]() {
            if (seq != m_commandSequence || !m_driver || !m_driver->isOpen()) return;
            fn();
        });
    };

    schedule(0, [this]() { m_driver->disableOrtho(); });
    schedule(40, [this, s]() {
        if (s.direction == QStringLiteral("left")) m_driver->turnLeft(s.speed);
        else m_driver->turnRight(s.speed);
    });
    schedule(120, [this, s]() { m_driver->setOrthoLength(s.orthoLength); });
    schedule(160, [this, s]() {
        if (s.feedbackEnabled) m_driver->enableFeedback();
        else m_driver->disableFeedback();
    });
    schedule(200, [this]() {
        m_driver->enableOrtho();
        chkOrthoEnabled->setChecked(true);
    });
    return true;
}

void TurntableControlDialog::stopAndDisableOrtho()
{
    if (!m_driver) return;
    nextCommandSequence();
    if (!m_driver->isOpen()) {
        chkOrthoEnabled->setChecked(false);
        return;
    }
    m_driver->stop();
    chkOrthoEnabled->setChecked(false);
    const int seq = m_commandSequence;
    QTimer::singleShot(40, this, [this, seq]() {
        if (seq != m_commandSequence || !m_driver || !m_driver->isOpen()) return;
        m_driver->disableOrtho();
    });
}
