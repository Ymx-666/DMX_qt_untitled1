#ifndef TURNTABLEDRIVER_H
#define TURNTABLEDRIVER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>

class TurntableDriver : public QObject
{
    Q_OBJECT
public:
    explicit TurntableDriver(QObject *parent = nullptr);
    ~TurntableDriver();

    bool openPort(const QString &portName, int baudRate = 9600);
    void closePort();
    bool isOpen() const;

    void turnLeft(int speed);
    void turnRight(int speed);
    void stop();
    void enableOrtho();
    void disableOrtho();
    void setOrthoLength(int length);
    void enableFeedback();
    void disableFeedback();

signals:
    void angleUpdated(double angle);
    void logMessage(const QString &msg);

    // [新增] 圈速测量完毕信号，向外广播上一圈的精确耗时
    void lapTimeMeasured(double seconds);

private slots:
    void readData();

private:
    void sendCommand(unsigned char cmd1, unsigned char cmd2, unsigned char data1, unsigned char data2);
    void resetLapTimer(); // [新增] 重置计时的辅助函数

    QSerialPort *m_serialPort;
    QByteArray m_rxBuffer;

    // [新增] 圈速计算核心变量
    double m_prevAngle;
    qint64 m_lastCrossTime;
    bool m_lapTimerRunning;
};

#endif // TURNTABLEDRIVER_H
