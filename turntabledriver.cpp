#include "turntabledriver.h"
#include <cmath>
#include <QDateTime> // 引入时间戳，用于计算精准圈速

TurntableDriver::TurntableDriver(QObject *parent) : QObject(parent)
{
    m_serialPort = new QSerialPort(this);
    connect(m_serialPort, &QSerialPort::readyRead, this, &TurntableDriver::readData);

    // 初始化圈速计算变量
    m_prevAngle = -1.0;
    m_lastCrossTime = 0;
    m_lapTimerRunning = false;
}

TurntableDriver::~TurntableDriver()
{
    closePort();
}

bool TurntableDriver::openPort(const QString &portName, int baudRate)
{
    if (m_serialPort->isOpen()) {
        m_serialPort->clear();
        m_serialPort->close();
    }
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(baudRate);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);

    if (m_serialPort->open(QIODevice::ReadWrite)) {
        m_rxBuffer.clear();
        emit logMessage("串口已开启: " + portName);
        return true;
    }
    return false;
}

void TurntableDriver::closePort()
{
    if (m_serialPort->isOpen()) {
        m_serialPort->clear();
        m_serialPort->close();
        emit logMessage("串口已关闭");
    }
}

bool TurntableDriver::isOpen() const { return m_serialPort->isOpen(); }

// ================= 底层解析与圈速计算核心 =================
void TurntableDriver::readData()
{
    QByteArray newData = m_serialPort->readAll();
    if (newData.isEmpty()) return;

    m_rxBuffer.append(newData);

    while (m_rxBuffer.size() >= 7) {
        if ((unsigned char)m_rxBuffer.at(0) != 0xFF) {
            m_rxBuffer.remove(0, 1);
            continue;
        }

        int sum = 0;
        for (int i = 1; i <= 5; ++i) { sum += (unsigned char)m_rxBuffer.at(i); }
        if ((sum & 0xFF) != (unsigned char)m_rxBuffer.at(6)) {
            m_rxBuffer.remove(0, 1);
            continue;
        }

        QByteArray packet = m_rxBuffer.left(7);
        m_rxBuffer.remove(0, 7);
        unsigned char cmd2 = (unsigned char)packet[3];

        if (cmd2 == 0x59) {
            unsigned short panAngle = (unsigned short)(((unsigned char)packet[4] << 8) | (unsigned char)packet[5]);
            double realAngle = panAngle / 100.0;
            if (realAngle >= 360.0) realAngle = std::fmod(realAngle, 360.0);

            // ----------------------------------------------------
            // 【圈速计算逻辑】：过零点检测算法
            // ----------------------------------------------------
            if (m_prevAngle >= 0.0 && m_lapTimerRunning) {
                // 如果两帧之间角度跳变超过 300 度，说明跨过了 0/360 度临界点
                if (std::abs(realAngle - m_prevAngle) > 300.0) {
                    qint64 now = QDateTime::currentMSecsSinceEpoch();
                    // 如果不是第一圈（有了起点时间），则计算耗时
                    if (m_lastCrossTime > 0) {
                        double lapSeconds = (now - m_lastCrossTime) / 1000.0;
                        emit lapTimeMeasured(lapSeconds); // 把圈速广播出去！
                    }
                    m_lastCrossTime = now; // 记录当前过零点时间，作为下一圈的起点
                }
            }
            m_prevAngle = realAngle;
            // ----------------------------------------------------

            emit angleUpdated(realAngle);
        }
    }
}

void TurntableDriver::sendCommand(unsigned char cmd1, unsigned char cmd2, unsigned char data1, unsigned char data2)
{
    if (!m_serialPort->isOpen()) return;

    QByteArray packet;
    packet.resize(7);
    packet[0] = 0xFF;
    packet[1] = 0x01;
    packet[2] = cmd1;
    packet[3] = cmd2;
    packet[4] = data1;
    packet[5] = data2;

    packet[6] = (packet[1] + packet[2] + packet[3] + packet[4] + packet[5]) & 0xFF;

    m_serialPort->write(packet);
}

// ================= 运动控制与重置计时 =================
void TurntableDriver::resetLapTimer() {
    m_lapTimerRunning = true;
    m_lastCrossTime = 0; // 重置起点
}

void TurntableDriver::turnLeft(int speed)  { sendCommand(0x00, 0x04, speed & 0xFF, 0x00); resetLapTimer(); }
void TurntableDriver::turnRight(int speed) { sendCommand(0x00, 0x02, speed & 0xFF, 0x00); resetLapTimer(); }
void TurntableDriver::stop()               { sendCommand(0x00, 0x00, 0x00, 0x00); m_lapTimerRunning = false; }

void TurntableDriver::enableOrtho()        { sendCommand(0x01, 0xE9, 0x00, 0x01); }
void TurntableDriver::disableOrtho()       { sendCommand(0x01, 0xE9, 0x00, 0x00); }
void TurntableDriver::setOrthoLength(int length) { sendCommand(0x02, 0xE9, (length >> 8) & 0xFF, length & 0xFF); }
void TurntableDriver::enableFeedback()     { sendCommand(0x00, 0x09, 0x00, 0x06); }
void TurntableDriver::disableFeedback()    { sendCommand(0x00, 0x03, 0x00, 0x74); }
