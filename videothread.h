#ifndef VIDEOTHREAD_H
#define VIDEOTHREAD_H

#include <QThread>
#include <QImage>
#include <QUdpSocket>
#include <QMap>
#include <QByteArray>

#pragma pack(push, 1)
struct ImagePacketHeader {
    uint16_t headerCode;
    uint16_t imageType;
    uint32_t timestamp;
    uint32_t imageIndex;
    uint32_t totalSize;
    uint16_t blockIndex;
    uint16_t blockSize;
};
#pragma pack(pop)

class VideoThread : public QThread
{
    Q_OBJECT
public:
    explicit VideoThread(int type, QObject *parent = nullptr);
    ~VideoThread();

    void stop();

signals:
    void frameCaptured(QImage img, int fileIndex);
    void thermalFrameCaptured(QImage img, int fileIndex);
    void pageTablePathReceived(QString path);

    // 【新增】：用于子线程向主界面的日志框发送系统状态
    void logRequested(const QString &type, const QString &msg, const QString &color);

protected:
    void run() override;

private slots:
    void processPendingDatagrams();
    void processPathDatagrams();

private:
    int m_type;
    bool m_running;

    QUdpSocket *m_dataSocket;
    QUdpSocket *m_pathSocket;

    struct ImageBuffer {
        uint32_t totalSize = 0;
        int receivedBytes = 0;
        QByteArray data;
    };
    QMap<uint32_t, ImageBuffer> m_bufferPool;
};

#endif // VIDEOTHREAD_H
