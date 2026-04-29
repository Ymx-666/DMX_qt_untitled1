#ifndef VIDEOTHREAD_H
#define VIDEOTHREAD_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QUdpSocket>
#include <QMap>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QReadWriteLock>

struct ImagePacketHeader;

class VideoWorker : public QObject
{
    Q_OBJECT
public:
    explicit VideoWorker(int type);
    ~VideoWorker();

public slots:
    void start();
    void stop();
    void requestRoi(double angleDeg, int tag);
    void requestPanoramaSnapshot();
    void enqueuePath(QString typeStr, QString pathStr, QString sender);

signals:
    void frameCaptured(QImage img, double angleDeg);
    void thermalFrameCaptured(QImage img, double angleDeg);
    void pathReceived(const QString &type, const QString &path, const QString &sender);
    void roiCaptured(QImage img, int tag);
    void panoramaSnapshotReady(QImage img);
    void logRequested(const QString &type, const QString &msg, const QString &color);

private slots:
    void processPendingDatagrams();
    void processPathDatagrams();
    void onStatTick();

private:
    int m_type;
    bool m_running = false;

    QUdpSocket *m_dataSocket = nullptr;
    QUdpSocket *m_pathSocket = nullptr;
    QTimer *m_statTimer = nullptr;

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

    struct ImageBuffer {
        uint32_t totalSize = 0;
        uint32_t receivedBytes = 0;
        QByteArray data;
        qint64 createdMs = 0;
        qint64 lastUpdateMs = 0;
        uint32_t lastProgressBytes = 0;
    };
    QMap<uint32_t, ImageBuffer> m_bufferPool;
    uint32_t m_rawMaxIndexSeen = 0;
    quint64 m_rawRxCounter = 0;

    QImage m_panorama;
    int m_fullSliceW = 0;
    int m_fullSliceH = 0;

    QVector<QReadWriteLock*> m_segLocks;
    int m_lockBuckets = 64;
};

class VideoThread : public QThread
{
    Q_OBJECT
public:
    explicit VideoThread(int type, QObject *parent = nullptr);
    ~VideoThread();

    void stop();

signals:
    void frameCaptured(QImage img, double angleDeg);
    void thermalFrameCaptured(QImage img, double angleDeg);
    void pathReceived(const QString &type, const QString &path, const QString &sender);
    void roiCaptured(QImage img, int tag);
    void panoramaSnapshotReady(QImage img);

    // 【新增】：用于子线程向主界面的日志框发送系统状态
    void logRequested(const QString &type, const QString &msg, const QString &color);

protected:
    void run() override;

public slots:
    void requestRoi(double angleDeg, int tag);
    void requestPanoramaSnapshot();
    void enqueuePath(QString typeStr, QString pathStr, QString sender);

private slots:
    void onWorkerLogRequested(const QString &type, const QString &msg, const QString &color);

private:
    int m_type;
    bool m_running;
    VideoWorker *m_worker = nullptr;
};

#endif // VIDEOTHREAD_H
