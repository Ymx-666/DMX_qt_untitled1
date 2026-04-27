#ifndef VIDEOTHREAD_H
#define VIDEOTHREAD_H

#include <QThread>
#include <QImage>
#include <QUdpSocket>
#include <QMap>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QQueue>

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
    void frameCaptured(QImage img, const QString &path);
    void thermalFrameCaptured(QImage img, const QString &path);
    void pageTablePathReceived(QString path);
    void pathJobReceived(const QString &type, const QString &path);

    // 【新增】：用于子线程向主界面的日志框发送系统状态
    void logRequested(const QString &type, const QString &msg, const QString &color);

protected:
    void run() override;

public slots:
    void enqueuePathJob(const QString &type, const QString &path);

private slots:
    void processPendingDatagrams();
    void processPathDatagrams();

private:
    int m_type;
    bool m_running;

    QUdpSocket *m_dataSocket;
    QUdpSocket *m_pathSocket;
    uint32_t m_textFrameIndex = 0;
    uint32_t m_pathRgbFrameIndex = 0;
    uint32_t m_pathBwFrameIndex = 0;
    QElapsedTimer m_emitTimer;
    qint64 m_lastTextEmitMs = 0;
    qint64 m_lastStatMs = 0;

    quint64 m_totalRxPackets = 0;
    quint64 m_totalDecodedFrames = 0;
    quint64 m_totalDroppedPackets = 0;
    quint64 m_totalReadFails = 0;

    quint64 m_lastStatRxPackets = 0;
    quint64 m_lastStatDecodedFrames = 0;
    quint64 m_lastStatDroppedPackets = 0;
    quint64 m_lastStatReadFails = 0;

    quint64 m_totalReadyReadCalls = 0;
    quint64 m_totalDatagramsRead = 0;
    int m_lastDatagramLen = 0;
    QString m_lastSender;

    QString m_lastRxType;
    QString m_lastRxPath;
    QString m_pendingType;
    QString m_pendingPath;
    bool m_pendingDirty = false;

    struct PathJob {
        QString type;
        QString path;
        qint64 nextTryMs = 0;
        qint64 lastSize = -1;
        int stableHits = 0;
        int attempts = 0;
    };
    QQueue<PathJob> m_rgbJobs;
    QQueue<PathJob> m_bwJobs;
    bool m_nextDecodeRgb = true;

    struct ImageBuffer {
        uint32_t totalSize = 0;
        uint32_t receivedBytes = 0;
        QByteArray data;
    };
    QMap<uint32_t, ImageBuffer> m_bufferPool;
};

#endif // VIDEOTHREAD_H
