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
#include <QBitArray>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

struct ImagePacketHeader;
class AppConfig;
class QFile;
class QHostAddress;
class PanoramaCache;

struct DetectCandidateLocal {
    int x = 0;
    int y = 0;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int area = 0;
    double cheapScore = 0.0;
    double score = 0.0;
    double response = 0.0;
    double contrast = 0.0;
    double centerRing = 0.0;
    double templateCorr = 0.0;
    double clutter = 0.0;
    double aspect = 1.0;
    double compactness = 1.0;
    QString detector = QStringLiteral("classic");
    int classId = -1;
    QString className;
    double yoloScore = 0.0;
    double yoloBirdScore = 0.0;

    int frameX = 0;
    int frameY = 0;
    int frameBoxX1 = 0;
    int frameBoxY1 = 0;
    int frameBoxX2 = 0;
    int frameBoxY2 = 0;
    int panoX = 0;
    int panoY = 0;
    int panoBoxX1 = 0;
    int panoBoxY1 = 0;
    int panoBoxX2 = 0;
    int panoBoxY2 = 0;
    int roiBoxX1 = 0;
    int roiBoxY1 = 0;
    int roiBoxX2 = 0;
    int roiBoxY2 = 0;
    int tileIndex = 0;
    int frameW = 0;
    int frameH = 0;
    int sliceW = 0;
    int horizonY = 0;
    int skyBottom = 0;
    quint64 round = 0;
    quint64 fileIdx = 0;
    qint64 rxMs = 0;
    QString sourcePath;
};

struct DetectRoundStateLocal {
    quint64 round = 0;
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    int frameW = 0;
    int frameH = 0;
    QVector<DetectCandidateLocal> candidates;
};

struct DetectSkyMaskBuildResultLocal {
    bool ready = false;
    QString error;
    cv::Mat rawMaskSmall;
    cv::Mat maskSmall;
    bool geometryCleanupApplied = false;
    qint64 generatedAtMs = 0;
    qint64 elapsedMs = 0;
    int cleanupInputPixels = 0;
    int cleanupTopConnectedPixels = 0;
    int cleanupNeckAnchoredPixels = 0;
    int cleanupRoughBoundaryPixels = 0;
    int cleanupOutputPixels = 0;
#ifdef DMX_ADVANCED_DETECTION
    bool runtimeMaskWriteAttempted = false;
    QString runtimeMaskPath;
    QString runtimeMaskError;
#endif
    bool previewAttempted = false;
    bool previewExported = false;
    QString previewDir;
    QString previewError;
};

struct DetectSkyMaskArchiveResultLocal {
    bool archived = false;
    QString archiveDir;
    QString error;
    qint64 elapsedMs = 0;
};

struct DetectSkyMaskPanoramaStateLocal {
    bool ready = false;
    bool readyLogged = false;
    quint64 readyRound = 0;
    int required = 0;
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    int smallW = 0;
    int smallH = 0;
    int smallSliceW = 0;
    QVector<cv::Mat> smallFrames;
    QVector<QBitArray> seenTiles;
    cv::Mat rawMaskSmall;
    cv::Mat maskSmall;
    bool buildStarted = false;
    quint64 buildRound = 0;
    std::shared_ptr<std::future<DetectSkyMaskBuildResultLocal>> buildFuture;
    bool archiveStarted = false;
    std::shared_ptr<std::future<DetectSkyMaskArchiveResultLocal>> archiveFuture;
    bool archiveAttempted = false;
    bool geometryCleanupApplied = false;
    qint64 generatedAtMs = 0;
#ifdef DMX_ADVANCED_DETECTION
    QString runtimeMaskPath;
#endif
};

class VideoWorker : public QObject
{
    Q_OBJECT
public:
    explicit VideoWorker(int type, QSharedPointer<PanoramaCache> cache = {});
    ~VideoWorker();

    struct DetectBackgroundTile {
        cv::Mat accum32f;
        cv::Mat background8;
        cv::Mat skyMask;
        QVector<cv::Mat> initialFrames;
        QVector<int> horizonProfile;
        int samples = 0;
        int horizonY = 0;
        bool ready = false;
    };

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
    void candidateDetected(const QString &stream, double angle, int panoX, int panoY, double score, const QString &cropPath,
                           int roiBoxX1, int roiBoxY1, int roiBoxX2, int roiBoxY2,
                           const QString &className);
#ifdef DMX_ADVANCED_DETECTION
    void directYoloFrameReady(const QString &stream,
                              const QString &sourcePath,
                              double angleDeg,
                              quint64 fileIdx,
                              qint64 rxMs,
                              int tileIndex,
                              int segments,
                              int sliceW,
                              int panoW,
                              int panoH,
                              const QString &skyMaskPath,
                              int skyShrinkPixels);
#endif

private slots:
    void processPathDatagrams();
    void onStatTick();
    void processOnePathJob();

private:
    struct PathJob;
    bool handlePathInternal(PathJob &job, int *retryMs);
    void collectTileCandidates(const QString &stream, const QImage &frameRgb32, double angleDeg, quint64 fileIdx, qint64 rxMs, const QString &sourcePath);
    void maybeFinalizeRoundCandidates(const QString &stream, quint64 fileIdx, qint64 rxMs, const QString &sourcePath);
    void finalizeRoundCandidates(const QString &stream, quint64 round, quint64 fileIdx, qint64 rxMs, const QString &sourcePath);
    cv::Mat updateAndGetPanoramaSkyMaskSlice(const QString &stream, const cv::Mat &gray, int tileIndex,
                                             int segments, int sliceW, int panoW, int panoH,
                                             quint64 totalFrames, const AppConfig &cfg);
    void recordRawOnly(PathJob &job);
    void noteReadFail(const QString &subType, const QString &detail, const QString &senderIp, qint64 nowMs);
    void updateSeqState(const QString &subType, quint64 fileIdx, const QString &winPath, qint64 nowMs);
    void schedulePathJobs(int delayMs);
    bool ensureRawUdpLogFile(qint64 rxMs);
    void writeRawUdpLog(const QByteArray &datagram, const QHostAddress &sender, quint16 senderPort, qint64 rxMs);
    void closeRawUdpLogFile();

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

    QFile *m_rawUdpLogFile = nullptr;
    QString m_rawUdpLogSessionName;
    QString m_rawUdpLogBucket;
    QString m_rawUdpLogPath;
    bool m_rawUdpLogDisabled = false;
    bool m_rawUdpLogErrorLogged = false;
    quint64 m_rawUdpLogCount = 0;

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

    QVector<DetectBackgroundTile> m_detectBgTiles;
    QString m_detectBgStream;
    int m_detectBgSegments = 0;
    int m_detectBgWidth = 0;
    int m_detectBgHeight = 0;
    int m_detectBgRequired = 0;
    int m_detectBgReadyTiles = 0;
    bool m_detectBgAllReadyLogged = false;
    DetectRoundStateLocal m_roundCandidatesRgb;
    DetectRoundStateLocal m_roundCandidatesBw;
    DetectSkyMaskPanoramaStateLocal m_skyMaskPanoramaRgb;
    DetectSkyMaskPanoramaStateLocal m_skyMaskPanoramaBw;
    quint64 m_lastFinalizedRoundRgb = 0;
    quint64 m_lastFinalizedRoundBw = 0;

    bool ensureYoloNet(const AppConfig &cfg);
    bool ensureTraditionalTemplate(const AppConfig &cfg);
    cv::dnn::Net m_yoloNet;
    QString m_yoloModelPath;
    bool m_yoloLoadAttempted = false;
    bool m_yoloReady = false;
    cv::Mat m_traditionalTemplate;
    QString m_traditionalTemplatePath;
    bool m_traditionalTemplateLoadAttempted = false;

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
    void candidateDetected(const QString &stream, double angle, int panoX, int panoY, double score, const QString &cropPath,
                           int roiBoxX1, int roiBoxY1, int roiBoxX2, int roiBoxY2,
                           const QString &className);
#ifdef DMX_ADVANCED_DETECTION
    void directYoloFrameReady(const QString &stream,
                              const QString &sourcePath,
                              double angleDeg,
                              quint64 fileIdx,
                              qint64 rxMs,
                              int tileIndex,
                              int segments,
                              int sliceW,
                              int panoW,
                              int panoH,
                              const QString &skyMaskPath,
                              int skyShrinkPixels);
#endif

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
