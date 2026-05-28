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
#include <QMutex>
#include <QSharedPointer>
#include <QSet>
#include <QQueue>
#include <QAtomicInteger>
#include <QPointer>
#include <cstdint>

struct ImagePacketHeader;
class PanoramaCache;

class VideoWorker : public QObject
{
    Q_OBJECT
public:
    explicit VideoWorker(int type, QSharedPointer<PanoramaCache> cache = {});
    ~VideoWorker();

public slots:
    void start();
    void stop();
    void setCurrentAngle(double angleDeg);
    void enqueuePath(QString typeStr, QString pathStr, QString sender, double angleDeg, qint64 rxMs);
    void setRecorder(QObject *recorder);
    void setRecordingEnabled(bool enabled);

signals:
    void pathReceived(const QString &type, const QString &path, const QString &sender, qint64 rxMs);
    void logRequested(const QString &type, const QString &msg, const QString &color);
    void cacheUpdated();

private slots:
    void processPathDatagrams();
    void onStatTick();
    void processOnePathJob();

private:
    struct PathJob;
    bool handlePathInternal(PathJob &job, int *retryMs);
    void recordRawOnly(PathJob &job);
    void noteReadFail(const QString &subType, const QString &detail, const QString &senderIp, qint64 nowMs);
    void updateSeqState(const QString &subType, quint64 fileIdx, const QString &winPath, qint64 nowMs);
    void schedulePathJobs(int delayMs);

    int m_type;
    bool m_running = false;

    QUdpSocket *m_pathSocket = nullptr;
    QTimer *m_statTimer = nullptr;


    struct SeqState {
        quint64 expected = 0;
        quint64 dupCount = 0;
        quint64 reorderCount = 0;
        quint64 gapCount = 0;
        quint64 maxGap = 0;
        quint64 maxReorder = 0;
    };
    SeqState m_seqRgb;
    SeqState m_seqBw;
    QElapsedTimer m_emitTimer;
    qint64 m_lastTextEmitMs = 0;
    qint64 m_lastStatMs = 0;

    quint64 m_totalRxPackets = 0;
    quint64 m_totalDecodedFrames = 0;
    quint64 m_totalDroppedPackets = 0;
    quint64 m_totalReadFails = 0;
    quint64 m_rxRgb = 0;
    quint64 m_rxBw = 0;

    quint64 m_handleMsAccum = 0;
    quint64 m_handleMsMax = 0;
    quint64 m_handleCount = 0;
    bool m_loggedFormat = false;

    quint64 m_lastStatRxPackets = 0;
    quint64 m_lastStatDecodedFrames = 0;
    quint64 m_lastStatDroppedPackets = 0;
    quint64 m_lastStatReadFails = 0;
    quint64 m_lastStatRxRgb = 0;
    quint64 m_lastStatRxBw = 0;

    quint64 m_totalReadyReadCalls = 0;
    quint64 m_totalDatagramsRead = 0;
    int m_lastDatagramLen = 0;
    QString m_lastSender;

    qint64 m_lastReadFailEmitMs = 0;
    quint64 m_readFailBurst = 0;
    QString m_lastReadFailDetail;

    QString m_lastRxType;
    QString m_lastRxPath;
    QString m_pendingType;
    QString m_pendingPath;
    bool m_pendingDirty = false;

    struct PathJob {
        QString type;
        QString path;
        QString sender;
        double angleDeg = 0.0;
        qint64 rxMs = 0;
        qint64 firstSeenMs = 0;
        qint64 lastSize = -1;
        int tries = 0;
        bool seqCounted = false; // count rx/seq once per frame, not per retry
    };
    QMutex m_jobsMtx;
    QQueue<PathJob> m_jobs;
    PathJob m_currentJob;
    bool m_hasCurrentJob = false;
    QAtomicInteger<int> m_jobsScheduled = 0;
    QTimer *m_jobsTimer = nullptr;

    // UI frame-drop valve: when the decode worker falls behind the device send
    // rate, the render/panorama path keeps only the newest frame and drops the
    // stale backlog so the UI stays live. Dropped frames are NOT lost from raw
    // recording: when recording is on, their raw bytes are still persisted
    // (recordRawOnly) before being skipped for decode. Disable via DMX_UI_NODROP=1.
    bool m_dropStaleForUi = true;
    int m_uiQueueCap = 2;
    quint64 m_lastRecordedIdx = 0;

    QSharedPointer<PanoramaCache> m_cache;
    QPointer<QObject> m_recorder;
    bool m_recordingEnabled = false;

    double m_currentAngleDeg = 0.0;
    double m_prevAngleDeg = 0.0;
    bool m_hasAngle = false;
    qint64 m_lastAngleChangeMs = 0;
};

class VideoThread : public QThread
{
    Q_OBJECT
public:
    explicit VideoThread(int type, QSharedPointer<PanoramaCache> cache = {}, QObject *parent = nullptr);
    ~VideoThread();

    void stop();

signals:
    void pathReceived(const QString &type, const QString &path, const QString &sender, qint64 rxMs);

    // 【新增】：用于子线程向主界面的日志框发送系统状态
    void logRequested(const QString &type, const QString &msg, const QString &color);
    void cacheUpdated();

protected:
    void run() override;

public slots:
    void setCurrentAngle(double angleDeg);
    void enqueuePath(QString typeStr, QString pathStr, QString sender, double angleDeg, qint64 rxMs);
    void setRecorder(QObject *recorder);
    void setRecordingEnabled(bool enabled);

private slots:
    void onWorkerLogRequested(const QString &type, const QString &msg, const QString &color);

private:
    int m_type;
    bool m_running;
    VideoWorker *m_worker = nullptr;
    QSharedPointer<PanoramaCache> m_cache;
    QPointer<QObject> m_recorder;
    bool m_recordingEnabled = false;
};

#endif // VIDEOTHREAD_H
