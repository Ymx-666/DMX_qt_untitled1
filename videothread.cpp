#include "videothread.h"
#include "appconfig.h"
#include "panoramacache.h"
#include "rawrecorder.h"
#include "udpprotocol.h"
#include <QDebug>
#include <QVector>
#include <QStringList>
#include <QAbstractSocket>
#include <QCoreApplication>
#include <QTimer>
#include <QTime>
#include <QDateTime>
#include <QMetaObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QImageReader>
#include <QImageWriter>
#include <QBuffer>
#include <QThread>
#include <QtGlobal>
#include <QtMath>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

static inline QString u8s(const char *s) { return QString::fromUtf8(s); }

static double normalize360(double a)
{
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    return a;
}

static const QVector<QRgb>& grayColorTable()
{
    static QVector<QRgb> t;
    if (t.isEmpty()) {
        t.reserve(256);
        for (int c = 0; c < 256; ++c) t.push_back(qRgb(c, c, c));
    }
    return t;
}

// 把任意输入快速归一为 Indexed8 灰度，避免 convertToFormat 的逐像素调色板匹配。
//
// 设备方给的 BW jpg 经过实测是 3 通道 RGB jpg（R=G=B 的伪灰度），Qt 解码出 Format_RGB32。
// Qt 自带的 convertToFormat(Indexed8) 会逐像素做 luminance + palette matching，
// 对 4096x4096 图像约 60-100ms。手写循环只提取 R 通道（R==G==B），可省一半时间。
static QImage toIndexed8Gray(const QImage &src)
{
    if (src.isNull()) return src;
    if (src.format() == QImage::Format_Indexed8) {
        if (src.colorTable().isEmpty()) {
            QImage out = src;
            out.setColorTable(grayColorTable());
            return out;
        }
        return src;
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)
    if (src.format() == QImage::Format_Grayscale8) {
        QImage shallow(src.constBits(), src.width(), src.height(),
                       src.bytesPerLine(), QImage::Format_Indexed8);
        shallow.setColorTable(grayColorTable());
        return shallow.copy();
    }
#endif
    if (src.format() == QImage::Format_RGB32 || src.format() == QImage::Format_ARGB32 ||
        src.format() == QImage::Format_ARGB32_Premultiplied) {
        const int w = src.width();
        const int h = src.height();
        QImage out(w, h, QImage::Format_Indexed8);
        if (out.isNull()) return out;
        out.setColorTable(grayColorTable());
        for (int y = 0; y < h; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb*>(src.constScanLine(y));
            uchar *d = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                d[x] = (uchar)qRed(s[x]);
            }
        }
        return out;
    }
    QImage out = src.convertToFormat(QImage::Format_Indexed8, grayColorTable());
    if (out.colorTable().isEmpty()) out.setColorTable(grayColorTable());
    return out;
}

// Orient a device frame for the panorama: rotate CCW 90 degrees, and (when
// mirrorH is true) horizontally mirror the result in the SAME pixel pass so the
// stitched panorama matches the real scene (the device frame is flipped relative
// to the turntable sweep, which caused the seam/misalignment). Fusing the mirror
// into the rotation avoids a second full-image copy (BW path is the bottleneck).
static QImage rotateCCW90(const QImage &src, bool mirrorH)
{
    if (src.isNull()) return QImage();
    const int w = src.width();
    const int h = src.height();
    if (w <= 0 || h <= 0) return QImage();

    if (src.format() == QImage::Format_RGB32) {
        QImage dst(h, w, QImage::Format_RGB32);
        if (dst.isNull()) return QImage();
        for (int y = 0; y < h; ++y) {
            const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
            const int dx = mirrorH ? (h - 1 - y) : y;
            for (int x = 0; x < w; ++x) {
                const int dy = (w - 1 - x);
                QRgb *dstLine = reinterpret_cast<QRgb*>(dst.scanLine(dy));
                dstLine[dx] = srcLine[x];
            }
        }
        return dst;
    }

    if (src.format() == QImage::Format_Indexed8) {
        QImage dst(h, w, QImage::Format_Indexed8);
        if (dst.isNull()) return QImage();
        dst.setColorTable(src.colorTable().isEmpty() ? grayColorTable() : src.colorTable());
        for (int y = 0; y < h; ++y) {
            const uchar *srcLine = src.constScanLine(y);
            const int dx = mirrorH ? (h - 1 - y) : y;
            for (int x = 0; x < w; ++x) {
                const int dy = (w - 1 - x);
                uchar *dstLine = dst.scanLine(dy);
                dstLine[dx] = srcLine[x];
            }
        }
        return dst;
    }

    return rotateCCW90(src.convertToFormat(QImage::Format_RGB32), mirrorH);
}

static QString extractSenderIp(const QString &sender)
{
    const QString s = sender.trimmed();
    if (s.isEmpty()) return QString();

    if (s.startsWith('[')) {
        const int r = s.indexOf(']');
        if (r > 1) return s.mid(1, r - 1);
    }

    const int colon = s.lastIndexOf(':');
    if (colon > 0) return s.left(colon);
    return s;
}

static int oddKernel(int k, int minValue)
{
    if (k < minValue) k = minValue;
    if ((k % 2) == 0) ++k;
    return k;
}

static int clampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double normalizeAngleLocal(double a)
{
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    return a;
}

static QString mapDevicePathToWindowsShare(QString path, const QString &senderIp)
{
    if (path.startsWith("file://", Qt::CaseInsensitive)) {
        path = path.mid(QString("file://").length());
    }

    if (path.startsWith("/data/")) {
#ifdef Q_OS_WIN
        const QString ip = senderIp.isEmpty() ? QString("192.168.4.1") : senderIp;
        path = "\\\\" + ip + "\\data\\" + path.mid(QString("/data/").length());
        path.replace("/", "\\");
#else
        Q_UNUSED(senderIp);
        QString mount = AppConfig::instance().shareMount;
        if (mount.isEmpty()) mount = QStringLiteral("/mnt/dmx_share");
        path = mount + path.mid(QString("/data").length());
#endif
    }

    return path;
}

static bool parsePathPayload(const QString &msg, QString *typeStr, QString *pathStr)
{
    if (!typeStr || !pathStr) return false;
    const QStringList parts = msg.split(";");
    if (parts.size() < 2) return false;

    const QString t = parts[0].trimmed().toUpper();
    const QString p = parts[1].trimmed();
    if (t.isEmpty() || p.isEmpty()) return false;

    *typeStr = t;
    *pathStr = p;
    return true;
}

static bool replaceTrailingIndex(const QString &path, quint64 newIdx, QString *outPath)
{
    if (!outPath) return false;
    const QFileInfo fi(path);
    const QString base = fi.completeBaseName();
    int i = base.size() - 1;
    while (i >= 0 && base[i].isDigit()) --i;
    if (i == base.size() - 1) return false;
    const QString head = base.left(i + 1);
    const QString ext = fi.completeSuffix();
    const QString file = ext.isEmpty() ? (head + QString::number(newIdx)) : (head + QString::number(newIdx) + "." + ext);
    *outPath = QDir(fi.path()).filePath(file);
    return true;
}

static bool waitForReadableFile(const QString &path, int maxWaitMs, qint64 *finalSize)
{
    if (finalSize) *finalSize = 0;
    if (path.isEmpty()) return false;

    QElapsedTimer t;
    t.start();

    qint64 lastSize = -1;
    while (t.elapsed() <= maxWaitMs) {
        const QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            const qint64 s = fi.size();
            if (s > 0 && s == lastSize) {
                if (finalSize) *finalSize = s;
                return true;
            }
            lastSize = s;
        }
        QThread::msleep(25);
    }
    return false;
}

static bool readImageWithRetry(const QString &path, bool preferGray, int maxWaitMs, QImage *outImg, QString *err)
{
    if (outImg) *outImg = QImage();
    if (err) err->clear();
    if (!outImg) return false;
    if (path.isEmpty()) {
        if (err) *err = "path empty";
        return false;
    }

    QElapsedTimer t;
    t.start();

    QString lastErr;
    while (t.elapsed() <= maxWaitMs) {
        const QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile() || fi.size() <= 0) {
            QThread::msleep(25);
            continue;
        }

        {
            QImageReader reader(path);
            QImage img = reader.read();
            if (!img.isNull()) {
                if (preferGray) {
                    if (img.format() != QImage::Format_Indexed8) img = img.convertToFormat(QImage::Format_Indexed8, grayColorTable());
                    if (img.colorTable().isEmpty()) img.setColorTable(grayColorTable());
                } else {
                    if (img.format() != QImage::Format_RGB32) img = img.convertToFormat(QImage::Format_RGB32);
                }
                *outImg = img;
                return true;
            }
            lastErr = reader.errorString();
        }

        QThread::msleep(30);
    }

    if (err) *err = lastErr.isEmpty() ? QString("decode timeout") : lastErr;
    return false;
}

static int bucketIndex(int tileIndex, int bucketCount)
{
    if (bucketCount <= 0) return 0;
    if (tileIndex < 0) tileIndex = 0;
    return tileIndex % bucketCount;
}

static void lockTwoWrite(QReadWriteLock *a, QReadWriteLock *b)
{
    if (!a && !b) return;
    if (a == b) {
        if (a) a->lockForWrite();
        return;
    }
    const uintptr_t pa = reinterpret_cast<uintptr_t>(a);
    const uintptr_t pb = reinterpret_cast<uintptr_t>(b);
    if (pa < pb) {
        if (a) a->lockForWrite();
        if (b) b->lockForWrite();
    } else {
        if (b) b->lockForWrite();
        if (a) a->lockForWrite();
    }
}

static void unlockTwoWrite(QReadWriteLock *a, QReadWriteLock *b)
{
    if (!a && !b) return;
    if (a == b) {
        if (a) a->unlock();
        return;
    }
    if (a) a->unlock();
    if (b) b->unlock();
}

static void lockTwoRead(QReadWriteLock *a, QReadWriteLock *b)
{
    if (!a && !b) return;
    if (a == b) {
        if (a) a->lockForRead();
        return;
    }
    const uintptr_t pa = reinterpret_cast<uintptr_t>(a);
    const uintptr_t pb = reinterpret_cast<uintptr_t>(b);
    if (pa < pb) {
        if (a) a->lockForRead();
        if (b) b->lockForRead();
    } else {
        if (b) b->lockForRead();
        if (a) a->lockForRead();
    }
}

static void unlockTwoRead(QReadWriteLock *a, QReadWriteLock *b)
{
    if (!a && !b) return;
    if (a == b) {
        if (a) a->unlock();
        return;
    }
    if (a) a->unlock();
    if (b) b->unlock();
}

VideoWorker::VideoWorker(int type, QSharedPointer<PanoramaCache> cache)
    : QObject(nullptr), m_type(type), m_cache(std::move(cache))
{
}

VideoWorker::~VideoWorker()
{
    stop();
    closeRawUdpLogFile();
}

void VideoWorker::start()
{
    m_running = true;
    m_dropStaleForUi = AppConfig::instance().uiDropStale;
    m_uiQueueCap = AppConfig::instance().uiQueueCap;
    m_lastRecordedIdx = 0;
    m_emitTimer.start();
    m_lastTextEmitMs = 0;
    m_lastStatMs = 0;
    m_totalRxPackets = 0;
    m_totalDecodedFrames = 0;
    m_totalDroppedPackets = 0;
    m_totalReadFails = 0;
    m_rxRgb = 0;
    m_rxBw = 0;
    m_handleMsAccum = 0;
    m_handleMsMax = 0;
    m_handleCount = 0;
    m_totalReadyReadCalls = 0;
    m_totalDatagramsRead = 0;
    m_lastDatagramLen = 0;
    m_lastSender.clear();
    m_lastReadFailEmitMs = 0;
    m_readFailBurst = 0;
    m_lastReadFailDetail.clear();
    m_lastStatRxPackets = 0;
    m_lastStatDecodedFrames = 0;
    m_lastStatDroppedPackets = 0;
    m_lastStatReadFails = 0;
    m_lastStatRxRgb = 0;
    m_lastStatRxBw = 0;
    m_lastRxType.clear();
    m_lastRxPath.clear();
    m_pendingType.clear();
    m_pendingPath.clear();
    m_pendingDirty = false;
    closeRawUdpLogFile();
    m_rawUdpLogSessionName = QStringLiteral("%1_pid%2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")))
        .arg(QCoreApplication::applicationPid());
    m_rawUdpLogDisabled = false;
    m_rawUdpLogErrorLogged = false;
    m_rawUdpLogCount = 0;
    m_roundCandidatesRgb = DetectRoundStateLocal();
    m_roundCandidatesBw = DetectRoundStateLocal();
    m_skyMaskPanoramaRgb = DetectSkyMaskPanoramaStateLocal();
    m_skyMaskPanoramaBw = DetectSkyMaskPanoramaStateLocal();
    m_lastFinalizedRoundRgb = 0;
    m_lastFinalizedRoundBw = 0;
    m_jobsScheduled.storeRelease(0);
    {
        QMutexLocker lk(&m_jobsMtx);
        m_jobs.clear();
        m_hasCurrentJob = false;
    }
    if (m_jobsTimer) {
        m_jobsTimer->stop();
        m_jobsTimer->deleteLater();
        m_jobsTimer = nullptr;
    }
    m_seqRgb = SeqState();
    m_seqBw = SeqState();
    if (m_cache && (m_type == 0 || m_type == 1)) {
        if (m_type == 0) m_cache->resetRgb();
        if (m_type == 1) m_cache->resetBw();
    }

    emit logRequested(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), QString("Worker start PID=%1").arg(QCoreApplication::applicationPid()), "#00AAAA");
    {
        const QList<QByteArray> fmts = QImageReader::supportedImageFormats();
        QStringList list;
        list.reserve(fmts.size());
        for (const QByteArray &f : fmts) list.push_back(QString::fromLatin1(f));
        emit logRequested(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), QStringLiteral("ImageFormats=%1").arg(list.join(",")), "#00AAAA");
    }

    m_statTimer = new QTimer(this);
    const int statIntervalMs = 1000;
    m_statTimer->setInterval(statIntervalMs);
    connect(m_statTimer, &QTimer::timeout, this, &VideoWorker::onStatTick);
    m_statTimer->start();

    if (m_type == 2) {
        const quint16 pathPort = AppConfig::instance().pathPort;
        m_pathSocket = new QUdpSocket(this);
        m_pathSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        if (m_pathSocket->bind(QHostAddress::AnyIPv4, pathPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            emit logRequested(
                u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
                QString("Bind %1 ok local=%2:%3 rcvbuf=%4")
                    .arg(pathPort)
                    .arg(m_pathSocket->localAddress().toString())
                    .arg(m_pathSocket->localPort())
                    .arg(m_pathSocket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong()),
                "#00AAAA"
            );
        } else {
            emit logRequested(
                u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
                QString("Bind %1 failed err=%2").arg(pathPort).arg(m_pathSocket->errorString()),
                "#F44336");
        }
        connect(m_pathSocket, SIGNAL(readyRead()), this, SLOT(processPathDatagrams()), Qt::DirectConnection);
    }

}

void VideoWorker::schedulePathJobs(int delayMs)
{
    if (!m_running) return;
    if (m_jobsScheduled.testAndSetAcquire(0, 1)) {
        if (delayMs <= 0) {
            QMetaObject::invokeMethod(this, "processOnePathJob", Qt::QueuedConnection);
            return;
        }
        if (!m_jobsTimer) {
            m_jobsTimer = new QTimer(this);
            m_jobsTimer->setSingleShot(true);
            connect(m_jobsTimer, &QTimer::timeout, this, &VideoWorker::processOnePathJob);
        }
        m_jobsTimer->start(delayMs);
    }
}

void VideoWorker::processOnePathJob()
{
    m_jobsScheduled.storeRelease(0);
    if (!m_running) return;

    PathJob job;
    bool hasJob = false;
    QVector<PathJob> droppedJobs;
    {
        QMutexLocker lk(&m_jobsMtx);
        if (m_hasCurrentJob) {
            job = m_currentJob;
            m_hasCurrentJob = false;
            hasJob = true;
        } else if (!m_jobs.isEmpty()) {
            // Frame-drop valve: if the decode worker has fallen behind the device
            // send rate, collapse the backlog and keep only the newest frame for
            // the UI/decode path. The stale skipped frames are handed to the
            // record-only path below (raw recording stays complete).
            if (m_dropStaleForUi && m_jobs.size() > m_uiQueueCap) {
                while (m_jobs.size() > 1) droppedJobs.push_back(m_jobs.dequeue());
            }
            job = m_jobs.dequeue();
            hasJob = true;
        }
    }
    if (!hasJob) return;

    for (int i = 0; i < droppedJobs.size(); ++i) {
        ++m_totalDroppedPackets;     // shows up as drop=N in the RX stat line
        recordRawOnly(droppedJobs[i]); // no-op unless recording is enabled
    }

    int retryMs = 0;
    const bool done = handlePathInternal(job, &retryMs);
    if (!done && retryMs > 0) {
        {
            QMutexLocker lk(&m_jobsMtx);
            m_currentJob = job;
            m_hasCurrentJob = true;
        }
        schedulePathJobs(retryMs);
        return;
    }

    bool more = false;
    {
        QMutexLocker lk(&m_jobsMtx);
        more = !m_jobs.isEmpty();
    }
    if (more) schedulePathJobs(0);
}

void VideoWorker::stop()
{
    if (!m_running) {
        closeRawUdpLogFile();
        return;
    }
    m_running = false;
    if (m_statTimer) {
        m_statTimer->stop();
        m_statTimer->deleteLater();
        m_statTimer = nullptr;
    }
    if (m_pathSocket) {
        m_pathSocket->close();
        m_pathSocket->deleteLater();
        m_pathSocket = nullptr;
    }
    closeRawUdpLogFile();
}

void VideoWorker::setCurrentAngle(double angleDeg)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double a = normalize360(angleDeg);
    if (!m_hasAngle) {
        m_hasAngle = true;
        m_currentAngleDeg = a;
        m_prevAngleDeg = a;
        m_lastAngleChangeMs = nowMs;
        return;
    }
    const double diff = qAbs(normalize360(a - m_prevAngleDeg));
    const double d = (diff > 180.0) ? (360.0 - diff) : diff;
    m_currentAngleDeg = a;
    if (d > 0.1) {
        m_prevAngleDeg = a;
        m_lastAngleChangeMs = nowMs;
    }
}

void VideoWorker::onStatTick()
{
    const int port = m_pathSocket ? (int)m_pathSocket->localPort() : (int)AppConfig::instance().pathPort;
    const quint64 rx = m_totalRxPackets - m_lastStatRxPackets;
    const quint64 ok = m_totalDecodedFrames - m_lastStatDecodedFrames;
    const quint64 drop = m_totalDroppedPackets - m_lastStatDroppedPackets;
    const quint64 fail = m_totalReadFails - m_lastStatReadFails;
    const quint64 rr = m_totalReadyReadCalls;
    const quint64 dg = m_totalDatagramsRead;
    m_lastStatRxPackets = m_totalRxPackets;
    m_lastStatDecodedFrames = m_totalDecodedFrames;
    m_lastStatDroppedPackets = m_totalDroppedPackets;
    m_lastStatReadFails = m_totalReadFails;

    const QString tail = (m_lastRxType.isEmpty() || m_lastRxPath.isEmpty())
        ? QString()
        : QString(" last=%1;%2").arg(m_lastRxType, mapDevicePathToWindowsShare(m_lastRxPath, extractSenderIp(m_lastSender)));
    const QString io = m_lastSender.isEmpty()
        ? QString()
        : QString(" rr=%1 dg=%2 lastdg=%3 from=%4").arg(rr).arg(dg).arg(m_lastDatagramLen).arg(m_lastSender);

    if (m_type == 2) {
        emit logRequested(QString("RX(%1)").arg(port), QString("pkt=%1%2%3").arg(rx).arg(io).arg(tail), "#00AAAA");
        const quint64 dr = m_rxRgb - m_lastStatRxRgb;
        const quint64 db = m_rxBw - m_lastStatRxBw;
        m_lastStatRxRgb = m_rxRgb;
        m_lastStatRxBw = m_rxBw;
        emit logRequested(
            QStringLiteral("RXTYPE"),
            QString("RGB=%1 BW=%2 total=%3").arg(dr).arg(db).arg(dr + db),
            QStringLiteral("#00AAAA"));
    } else {
        emit logRequested(
            QString("RX(%1)").arg(port),
            QString("pkt=%1 ok=%2 drop=%3 fail=%4%5%6").arg(rx).arg(ok).arg(drop).arg(fail).arg(io).arg(tail),
            "#6A9955"
        );
    }

    int pathJobCount = 0;
    {
        QMutexLocker lk(&m_jobsMtx);
        pathJobCount = m_jobs.size() + (m_hasCurrentJob ? 1 : 0);
    }
    QString recStr;
    if (m_type == 0) {
        if (RawRecorder *rec = qobject_cast<RawRecorder*>(m_recorder.data())) {
            int recCount = 0;
            qint64 recBytes = 0;
            rec->queueStats(&recCount, &recBytes);
            recStr = QString(" rec=%1 recBytes=%2KB").arg(recCount).arg(recBytes / 1024);
        }
    }
    emit logRequested(
        QStringLiteral("QUEUE"),
        QString("path=%1%2").arg(pathJobCount).arg(recStr),
        QStringLiteral("#00AAAA"));

    auto flushSeq = [&](const char *name, SeqState &st) {
        if (st.dupCount == 0 && st.reorderCount == 0 && st.gapCount == 0) return;
        emit logRequested(
            QStringLiteral("SEQ"),
            QString("%1 dup=%2 reorder=%3 gap=%4 maxGap=%5 maxReorder=%6")
                .arg(QString::fromLatin1(name))
                .arg(st.dupCount).arg(st.reorderCount).arg(st.gapCount)
                .arg(st.maxGap).arg(st.maxReorder),
            QStringLiteral("#FF9800"));
        st.dupCount = 0;
        st.reorderCount = 0;
        st.gapCount = 0;
        st.maxGap = 0;
        st.maxReorder = 0;
    };
    flushSeq("RGB", m_seqRgb);
    flushSeq("BW", m_seqBw);

    if (m_handleCount > 0) {
        const quint64 avg = m_handleMsAccum / m_handleCount;
        emit logRequested(
            QStringLiteral("FPS"),
            QString("frames=%1/s avg=%2ms max=%3ms").arg(m_handleCount).arg(avg).arg(m_handleMsMax),
            QStringLiteral("#FFD54F"));
        m_handleMsAccum = 0;
        m_handleMsMax = 0;
        m_handleCount = 0;
    }
}

void VideoWorker::noteReadFail(const QString &subType, const QString &detail, const QString &senderIp, qint64 nowMs)
{
    ++m_readFailBurst;
    m_lastReadFailDetail = detail;
    if (m_lastReadFailEmitMs == 0 || (nowMs - m_lastReadFailEmitMs) >= 600) {
        const quint64 n = m_readFailBurst;
        m_readFailBurst = 0;
        m_lastReadFailEmitMs = nowMs;
        const QString msg = (n <= 1)
            ? QString("%1 %2").arg(subType, detail)
            : QString("%1 x%2 last: %3").arg(subType).arg(n).arg(detail);
        emit logRequested(u8s("\xE8\xAF\xBB\xE5\x8F\x96\xE5\xA4\xB1\xE8\xB4\xA5"), msg + (senderIp.isEmpty() ? QString() : QStringLiteral(" (from=%1)").arg(senderIp)), "#F44336");
    }
}

void VideoWorker::updateSeqState(const QString &subType, quint64 fileIdx, const QString &winPath, qint64 nowMs)
{
    Q_UNUSED(winPath);
    Q_UNUSED(nowMs);
    SeqState *st = nullptr;
    if (subType == "RGB") st = &m_seqRgb;
    if (subType == "BW") st = &m_seqBw;
    if (!st) return;
    if (fileIdx == 0) return;

    if (st->expected == 0) {
        st->expected = fileIdx + 1;
        return;
    }
    if (fileIdx == st->expected) {
        st->expected = fileIdx + 1;
        return;
    }
    if (fileIdx + 1 == st->expected) {
        ++st->dupCount;
        return;
    }
    if (fileIdx > st->expected) {
        const quint64 g = fileIdx - st->expected;
        ++st->gapCount;
        if (g > st->maxGap) st->maxGap = g;
        st->expected = fileIdx + 1;
        return;
    }
    const quint64 r = (st->expected - 1) - fileIdx;
    ++st->reorderCount;
    if (r > st->maxReorder) st->maxReorder = r;
}

bool VideoWorker::ensureRawUdpLogFile(qint64 rxMs)
{
    if (m_type != 2 || m_rawUdpLogDisabled) return false;

    const QString root = AppConfig::instance().rawLogRoot.trimmed();
    if (root.isEmpty()) {
        m_rawUdpLogDisabled = true;
        return false;
    }

    QDateTime dt = QDateTime::fromMSecsSinceEpoch(rxMs > 0 ? rxMs : QDateTime::currentMSecsSinceEpoch());
    if (!dt.isValid()) dt = QDateTime::currentDateTime();

    const QString day = dt.date().toString(QStringLiteral("yyyyMMdd"));
    const QString hour = dt.time().toString(QStringLiteral("HH"));
    const QString bucket = day + QStringLiteral("/") + hour;
    if (m_rawUdpLogFile && m_rawUdpLogFile->isOpen() && m_rawUdpLogBucket == bucket) return true;

    closeRawUdpLogFile();

    const QString outDir = QDir(root).filePath(bucket);
    if (!QDir().mkpath(outDir)) {
        if (!m_rawUdpLogErrorLogged) {
            m_rawUdpLogErrorLogged = true;
            emit logRequested(QStringLiteral("RAWLOG"), QStringLiteral("dir create failed: %1").arg(outDir), QStringLiteral("#F44336"));
        }
        return false;
    }

    if (m_rawUdpLogSessionName.isEmpty()) {
        m_rawUdpLogSessionName = QStringLiteral("%1_pid%2")
            .arg(dt.toString(QStringLiteral("yyyyMMdd_HHmmss")))
            .arg(QCoreApplication::applicationPid());
    }
    const int port = m_pathSocket ? (int)m_pathSocket->localPort() : (int)AppConfig::instance().pathPort;
    const QString path = QDir(outDir).filePath(
        QStringLiteral("udp_%1_%2_%3_%4.jsonl").arg(port).arg(day, hour, m_rawUdpLogSessionName));
    QFile *file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (!m_rawUdpLogErrorLogged) {
            m_rawUdpLogErrorLogged = true;
            emit logRequested(QStringLiteral("RAWLOG"), QStringLiteral("open failed: %1 (%2)").arg(path, file->errorString()), QStringLiteral("#F44336"));
        }
        delete file;
        return false;
    }

    m_rawUdpLogFile = file;
    m_rawUdpLogBucket = bucket;
    m_rawUdpLogPath = path;
    m_rawUdpLogErrorLogged = false;
    emit logRequested(QStringLiteral("RAWLOG"), QStringLiteral("%1 UDP text: %2").arg(port).arg(path), QStringLiteral("#6A9955"));
    return true;
}

void VideoWorker::writeRawUdpLog(const QByteArray &datagram, const QHostAddress &sender, quint16 senderPort, qint64 rxMs)
{
    if (!ensureRawUdpLogFile(rxMs)) return;

    QJsonObject rec;
    rec.insert(QStringLiteral("rxMs"), QString::fromLatin1(QByteArray::number(rxMs)));
    rec.insert(QStringLiteral("rxIso"), QDateTime::fromMSecsSinceEpoch(rxMs).toString(Qt::ISODateWithMs));
    rec.insert(QStringLiteral("sender"), QStringLiteral("%1:%2").arg(sender.toString()).arg(senderPort));
    rec.insert(QStringLiteral("len"), datagram.size());
    rec.insert(QStringLiteral("text"), QString::fromUtf8(datagram.constData(), datagram.size()));

    QByteArray line = QJsonDocument(rec).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (m_rawUdpLogFile->write(line) != line.size()) {
        if (!m_rawUdpLogErrorLogged) {
            m_rawUdpLogErrorLogged = true;
            emit logRequested(QStringLiteral("RAWLOG"), QStringLiteral("write failed: %1").arg(m_rawUdpLogPath), QStringLiteral("#F44336"));
        }
        return;
    }

    ++m_rawUdpLogCount;
    m_rawUdpLogFile->flush();
}

void VideoWorker::closeRawUdpLogFile()
{
    if (!m_rawUdpLogFile) return;
    if (m_rawUdpLogFile->isOpen()) {
        m_rawUdpLogFile->flush();
        m_rawUdpLogFile->close();
    }
    delete m_rawUdpLogFile;
    m_rawUdpLogFile = nullptr;
    m_rawUdpLogBucket.clear();
    m_rawUdpLogPath.clear();
}

VideoThread::VideoThread(int type, QSharedPointer<PanoramaCache> cache, QObject *parent)
    : QThread(parent),
      m_type(type),
      m_running(false),
      m_worker(nullptr),
      m_cache(std::move(cache))
{
    const int pathPort = (int)AppConfig::instance().pathPort;
    QString role;
    if (m_type == 1) role = QStringLiteral("%1(BW) path worker").arg(pathPort);
    else if (m_type == 0) role = QStringLiteral("%1(RGB) path worker").arg(pathPort);
    else role = QStringLiteral("%1 path dispatcher").arg(pathPort);
    qDebug() << "UDP worker ready:" << role;
}

VideoThread::~VideoThread()
{
    stop();
    wait();
}

void VideoThread::stop()
{
    m_running = false;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    }
    quit();
}

void VideoThread::setCurrentAngle(double angleDeg)
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(
        m_worker,
        "setCurrentAngle",
        Qt::QueuedConnection,
        Q_ARG(double, angleDeg)
    );
}

void VideoThread::enqueuePath(QString typeStr, QString pathStr, QString sender, double angleDeg, qint64 rxMs)
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(
        m_worker,
        "enqueuePath",
        Qt::QueuedConnection,
        Q_ARG(QString, typeStr),
        Q_ARG(QString, pathStr),
        Q_ARG(QString, sender),
        Q_ARG(double, angleDeg),
        Q_ARG(qint64, rxMs)
    );
}

void VideoThread::setRecorder(QObject *recorder)
{
    m_recorder = recorder;
    if (!m_worker) return;
    QMetaObject::invokeMethod(m_worker, "setRecorder", Qt::QueuedConnection, Q_ARG(QObject*, recorder));
}

void VideoThread::setRecordingEnabled(bool enabled)
{
    m_recordingEnabled = enabled;
    if (!m_worker) return;
    QMetaObject::invokeMethod(m_worker, "setRecordingEnabled", Qt::QueuedConnection, Q_ARG(bool, enabled));
}

void VideoThread::run()
{
    m_running = true;
    m_worker = new VideoWorker(m_type, m_cache);
    m_worker->moveToThread(this);
    m_worker->setRecorder(m_recorder.data());
    m_worker->setRecordingEnabled(m_recordingEnabled);
    connect(m_worker, &VideoWorker::pathReceived, this, &VideoThread::pathReceived, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::cacheUpdated, this, &VideoThread::cacheUpdated, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::candidateDetected, this, &VideoThread::candidateDetected, Qt::QueuedConnection);
#ifdef DMX_ADVANCED_DETECTION
    connect(m_worker, &VideoWorker::directYoloFrameReady, this, &VideoThread::directYoloFrameReady, Qt::QueuedConnection);
#endif
    connect(m_worker, &VideoWorker::logRequested, this, &VideoThread::onWorkerLogRequested, Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_worker, "start", Qt::DirectConnection);
    exec();
    if (m_worker) {
        m_worker->stop();
        delete m_worker;
        m_worker = nullptr;
    }
}

void VideoWorker::processPathDatagrams()
{
    if (!m_pathSocket) return;
    ++m_totalReadyReadCalls;
    while (m_pathSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_pathSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = m_pathSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        if (read <= 0) continue;
        if (read != datagram.size()) datagram.resize((int)read);
        ++m_totalDatagramsRead;
        m_lastDatagramLen = datagram.size();
        m_lastSender = QString("%1:%2").arg(sender.toString()).arg(senderPort);
        ++m_totalRxPackets;

        const qint64 rxMs = QDateTime::currentMSecsSinceEpoch();
        writeRawUdpLog(datagram, sender, senderPort, rxMs);
        const QString msg = QString::fromUtf8(datagram).trimmed();
        QString typeStr;
        QString originalPath;
        if (!parsePathPayload(msg, &typeStr, &originalPath)) continue;
        const QString typeUpper = typeStr.trimmed().toUpper();
        if (typeUpper == "RGB") {
            ++m_rxRgb;
        } else if (typeUpper == "BW" || typeUpper == "GRAY") {
            ++m_rxBw;
        }

        const QString senderStr = QString("%1:%2").arg(sender.toString()).arg(senderPort);
        if (m_type == 2) {
            emit pathReceived(typeUpper, originalPath.trimmed(), senderStr, rxMs);
            continue;
        }
        enqueuePath(typeStr, originalPath, senderStr, 0.0, rxMs);
    }
}

void VideoWorker::enqueuePath(QString typeStr, QString pathStr, QString sender, double angleDeg, qint64 rxMs)
{
    if (!m_running) return;
    PathJob j;
    j.type = std::move(typeStr);
    j.path = std::move(pathStr);
    j.sender = std::move(sender);
    j.angleDeg = angleDeg;
    j.rxMs = rxMs;
    {
        QMutexLocker lk(&m_jobsMtx);
        m_jobs.enqueue(std::move(j));
    }
    schedulePathJobs(0);
}

void VideoWorker::setRecorder(QObject *recorder)
{
    m_recorder = recorder;
}

void VideoWorker::setRecordingEnabled(bool enabled)
{
    m_recordingEnabled = enabled;
}

// Parse the trailing decimal run of a filename's base as the device frame index,
// e.g. "BW_20260528_210326_53496" -> 53496. Shared by the decode path and the
// record-only drop path.
static bool parseTrailingIndexStatic(const QString &path, quint64 *out)
{
    const QString base = QFileInfo(path).completeBaseName();
    int i = base.size() - 1;
    while (i >= 0 && base[i].isDigit()) --i;
    const QString digits = base.mid(i + 1);
    if (digits.isEmpty()) return false;
    bool ok = false;
    const quint64 v = digits.toULongLong(&ok);
    if (!ok) return false;
    if (out) *out = v;
    return true;
}

void VideoWorker::recordRawOnly(VideoWorker::PathJob &job)
{
    // Persist the raw bytes of a frame we are dropping from the UI/decode path so
    // raw recording stays complete. No decode / rotate / panorama push.
    if (!m_recordingEnabled || m_recorder.isNull()) return;
    const QString t = job.type.trimmed().toUpper();
    const QString p = job.path.trimmed();
    if (t.isEmpty() || p.isEmpty()) return;
    const QString senderIp = extractSenderIp(job.sender);
    const QString winPath = mapDevicePathToWindowsShare(p, senderIp);
    QFileInfo fi(winPath);
    if (!fi.exists() || !fi.isFile() || fi.size() <= 0) return; // dropped frames are old -> ready
    quint64 fileIdx = 0;
    parseTrailingIndexStatic(winPath, &fileIdx);
    if (fileIdx != 0 && fileIdx == m_lastRecordedIdx) return; // dedup exact resend
    QFile f(winPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) return;
    const QString subType = (t == "GRAY") ? QStringLiteral("BW") : t;
    const QString ext = fi.suffix().trimmed().toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(
        m_recorder.data(), "enqueueFrame", Qt::QueuedConnection,
        Q_ARG(QString, subType), Q_ARG(quint64, fileIdx), Q_ARG(qint64, nowMs),
        Q_ARG(QString, winPath), Q_ARG(QString, job.sender), Q_ARG(QString, ext), Q_ARG(QByteArray, bytes));
    if (fileIdx != 0) m_lastRecordedIdx = fileIdx;
}

struct YoloDetectionLocal {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int classId = -1;
    QString className;
    double confidence = 0.0;
    double weightedConfidence = 0.0;
};

struct YoloRunDiagnosticsLocal {
    QString outputShape;
    int outputRows = 0;
    int preNmsCount = 0;
    int nmsCount = 0;
    int resultCount = 0;
    int maxClass = -1;
    double maxConfidence = 0.0;
    double maxWeightedConfidence = 0.0;
    int maxX1 = 0;
    int maxY1 = 0;
    int maxX2 = 0;
    int maxY2 = 0;
};

struct SupplementWindowLocal {
    int x = 0;
    int y = 0;
    int size = 0;
    double score = 0.0;
};

static bool candidateFarEnough(const QVector<DetectCandidateLocal> &kept, const DetectCandidateLocal &c, int radius)
{
    if (radius <= 0) return true;
    const int r2 = radius * radius;
    for (const DetectCandidateLocal &k : kept) {
        const int dx = c.x - k.x;
        const int dy = c.y - k.y;
        if ((dx * dx + dy * dy) <= r2) return false;
    }
    return true;
}

static QVector<DetectCandidateLocal> spatialSelectCandidates(const QVector<DetectCandidateLocal> &sorted,
                                                             int limit,
                                                             int radius)
{
    QVector<DetectCandidateLocal> selected;
    if (limit <= 0) return selected;
    selected.reserve(qMin(limit, sorted.size()));
    for (const DetectCandidateLocal &c : sorted) {
        if (!candidateFarEnough(selected, c, radius)) continue;
        selected.push_back(c);
        if (selected.size() >= limit) break;
    }
    return selected;
}

static bool roundCandidateFarEnough(const QVector<DetectCandidateLocal> &kept,
                                    const DetectCandidateLocal &c,
                                    int radius,
                                    int panoW)
{
    if (radius <= 0) return true;
    const int r2 = radius * radius;
    for (const DetectCandidateLocal &k : kept) {
        int dx = qAbs(c.panoX - k.panoX);
        if (panoW > 0) dx = qMin(dx, qAbs(panoW - dx));
        const int dy = c.panoY - k.panoY;
        if ((dx * dx + dy * dy) <= r2) return false;
    }
    return true;
}

static QVector<DetectCandidateLocal> spatialSelectRoundCandidates(const QVector<DetectCandidateLocal> &sorted,
                                                                  int limit,
                                                                  int radius,
                                                                  int panoW)
{
    QVector<DetectCandidateLocal> selected;
    if (limit <= 0) return selected;
    selected.reserve(qMin(limit, sorted.size()));
    for (const DetectCandidateLocal &c : sorted) {
        if (!roundCandidateFarEnough(selected, c, radius, panoW)) continue;
        selected.push_back(c);
        if (selected.size() >= limit) break;
    }
    return selected;
}

static int wrapPanoX(int x, int panoW)
{
    if (panoW <= 0) return x;
    x %= panoW;
    if (x < 0) x += panoW;
    return x;
}

static int circularDeltaX(int x, int centerX, int panoW)
{
    int d = x - centerX;
    if (panoW <= 0) return d;
    const int half = panoW / 2;
    while (d > half) d -= panoW;
    while (d < -half) d += panoW;
    return d;
}

static void setRoiBoxFromPanoBox(DetectCandidateLocal *c, int cropSize, int panoW)
{
    if (!c || cropSize <= 0) return;
    const int half = cropSize / 2;
    c->roiBoxX1 = clampInt(half + circularDeltaX(c->panoBoxX1, c->panoX, panoW), 0, cropSize - 1);
    c->roiBoxY1 = clampInt(half + (c->panoBoxY1 - c->panoY), 0, cropSize - 1);
    c->roiBoxX2 = clampInt(half + circularDeltaX(c->panoBoxX2, c->panoX, panoW), 0, cropSize - 1);
    c->roiBoxY2 = clampInt(half + (c->panoBoxY2 - c->panoY), 0, cropSize - 1);
    if (c->roiBoxX2 <= c->roiBoxX1) c->roiBoxX2 = qMin(cropSize - 1, c->roiBoxX1 + 1);
    if (c->roiBoxY2 <= c->roiBoxY1) c->roiBoxY2 = qMin(cropSize - 1, c->roiBoxY1 + 1);
}

static int estimateHorizonY(const cv::Mat &gray)
{
    if (gray.empty()) return 0;
    const int h = gray.rows;
    const int w = gray.cols;
    if (h <= 16 || w <= 0) return h / 2;

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(0, 0), 3.0);

    int bestY = h / 2;
    double bestScore = -1.0;
    const int y0 = qMax(4, h / 8);
    const int y1 = qMin(h - 5, h * 7 / 8);
    for (int y = y0; y <= y1; ++y) {
        const double up = cv::mean(blurred.row(y - 3))[0];
        const double down = cv::mean(blurred.row(y + 3))[0];
        const double score = qAbs(down - up);
        if (score > bestScore) {
            bestScore = score;
            bestY = y;
        }
    }
    return clampInt(bestY, 0, h - 1);
}

static QVector<int> estimateHorizonProfile(const cv::Mat &gray, int fallbackY)
{
    QVector<int> profile;
    if (gray.empty()) return profile;
    const int h = gray.rows;
    const int w = gray.cols;
    profile.fill(clampInt(fallbackY, 0, qMax(0, h - 1)), w);
    if (h <= 16 || w <= 0) return profile;

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(0, 0), 3.0);
    cv::Mat gradY;
    cv::Sobel(blurred, gradY, CV_32F, 0, 1, 3);
    cv::Mat absGrad;
    cv::absdiff(gradY, cv::Scalar(0), absGrad);

    const int blockW = qBound(8, w / 96, 64);
    const int y0 = qMax(4, h / 8);
    const int y1 = qMin(h - 5, h * 7 / 8);
    const int maxJump = qMax(24, h / 6);
    QVector<int> blockYs;
    QVector<int> blockCenters;

    for (int x0 = 0; x0 < w; x0 += blockW) {
        const int x1 = qMin(w, x0 + blockW);
        int bestY = fallbackY;
        double bestScore = -1.0;
        for (int y = y0; y <= y1; ++y) {
            const int yy0 = qMax(0, y - 1);
            const int yy1 = qMin(h, y + 2);
            const cv::Rect roi(x0, yy0, x1 - x0, yy1 - yy0);
            const double score = cv::mean(absGrad(roi))[0];
            if (score > bestScore) {
                bestScore = score;
                bestY = y;
            }
        }
        blockYs << clampInt(bestY, fallbackY - maxJump, fallbackY + maxJump);
        blockCenters << (x0 + x1 - 1) / 2;
    }

    if (blockYs.isEmpty()) return profile;
    QVector<int> smooth = blockYs;
    for (int i = 0; i < blockYs.size(); ++i) {
        QVector<int> vals;
        for (int j = qMax(0, i - 2); j <= qMin(blockYs.size() - 1, i + 2); ++j) vals << blockYs.at(j);
        std::sort(vals.begin(), vals.end());
        smooth[i] = vals.at(vals.size() / 2);
    }

    int bi = 0;
    for (int x = 0; x < w; ++x) {
        while (bi + 1 < blockCenters.size() && x > blockCenters.at(bi + 1)) ++bi;
        if (bi + 1 >= blockCenters.size()) {
            profile[x] = smooth.at(bi);
        } else {
            const int xA = blockCenters.at(bi);
            const int xB = blockCenters.at(bi + 1);
            const double t = (xB == xA) ? 0.0 : (double)(x - xA) / (double)(xB - xA);
            profile[x] = clampInt((int)qRound(smooth.at(bi) * (1.0 - t) + smooth.at(bi + 1) * t), 0, h - 1);
        }
    }
    return profile;
}

static double percentileOfMat(const cv::Mat &mat, double percentile)
{
    if (mat.empty()) return 0.0;
    cv::Mat flat = mat.reshape(1, 1);
    cv::Mat flat32;
    flat.convertTo(flat32, CV_32F);
    std::vector<float> values;
    values.assign((float*)flat32.datastart, (float*)flat32.dataend);
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double p = qBound(0.0, percentile, 100.0) / 100.0;
    const int idx = qBound(0, (int)qRound(p * (values.size() - 1)), (int)values.size() - 1);
    return values[(size_t)idx];
}

struct SkyFeatureLocal {
    cv::Mat blur;
    cv::Mat grad;
    cv::Mat texture;
    cv::Mat lap;
};

struct SkyThresholdsLocal {
    double brightCandidate = 48.0;
    double brightSeed = 72.0;
    double cloudBright = 58.0;
    double gradCandidate = 7.0;
    double gradSeed = 4.0;
    double cloudGrad = 20.0;
    double textureCandidate = 5.0;
    double textureSeed = 2.0;
    double cloudTexture = 9.0;
    double lapCandidate = 5.0;
    double cloudLap = 12.0;
    double cloudEdgeDensity = 0.22;
};

static SkyFeatureLocal computeSkyFeatures(const cv::Mat &small)
{
    SkyFeatureLocal f;
    cv::GaussianBlur(small, f.blur, cv::Size(0, 0), 3.0);

    cv::Mat gradX, gradY;
    cv::Sobel(f.blur, gradX, CV_32F, 1, 0, 3);
    cv::Sobel(f.blur, gradY, CV_32F, 0, 1, 3);
    cv::magnitude(gradX, gradY, f.grad);

    cv::Laplacian(f.blur, f.lap, CV_32F, 3);
    cv::absdiff(f.lap, cv::Scalar(0), f.lap);

    cv::Mat blur32;
    f.blur.convertTo(blur32, CV_32F);
    cv::Mat mean, sqMean;
    cv::blur(blur32, mean, cv::Size(17, 17));
    cv::blur(blur32.mul(blur32), sqMean, cv::Size(17, 17));
    cv::Mat variance = sqMean - mean.mul(mean);
    cv::max(variance, 0.0, variance);
    cv::sqrt(variance, f.texture);
    return f;
}

static SkyThresholdsLocal computeSkyThresholds(const cv::Mat &firstSmall)
{
    SkyThresholdsLocal th;
    if (firstSmall.empty()) return th;
    const SkyFeatureLocal f = computeSkyFeatures(firstSmall);
    const int upperH = qMax(1, (int)qRound(firstSmall.rows * 0.60));
    const cv::Rect upper(0, 0, firstSmall.cols, upperH);
    th.brightSeed = qMin(115.0, qMax(72.0, percentileOfMat(f.blur(upper), 78.0)));
    th.brightCandidate = qMin(70.0, qMax(48.0, th.brightSeed - 28.0));
    th.cloudBright = qMax(42.0, 58.0 - qMax(0.0, th.brightSeed - 85.0) * 0.65);
    th.gradCandidate = qMin(18.0, qMax(7.0, percentileOfMat(f.grad(upper), 62.0)));
    th.gradSeed = qMin(8.0, qMax(3.0, percentileOfMat(f.grad(upper), 42.0)));
    th.cloudGrad = qMin(42.0, qMax(20.0, percentileOfMat(f.grad(upper), 86.0)));
    th.textureCandidate = qMin(8.0, qMax(2.8, percentileOfMat(f.texture(upper), 66.0)));
    th.textureSeed = qMin(4.5, qMax(1.2, percentileOfMat(f.texture(upper), 45.0)));
    th.cloudTexture = qMin(24.0, qMax(8.0, percentileOfMat(f.texture(upper), 86.0)));
    th.lapCandidate = qMin(18.0, qMax(5.0, percentileOfMat(f.lap(upper), 72.0)));
    th.cloudLap = qMin(36.0, qMax(12.0, percentileOfMat(f.lap(upper), 88.0)));
    th.cloudEdgeDensity = 0.22;
    return th;
}

static cv::Mat cutNarrowChannels(const cv::Mat &mask, int minWidth)
{
    if (mask.empty()) return mask;
    int k = qMax(3, minWidth);
    if ((k % 2) == 0) ++k;
    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    cv::Mat opened;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    cv::morphologyEx(binary, opened, cv::MORPH_OPEN, kernel);
    return opened;
}

static void buildFrameSkyCandidates(const cv::Mat &small,
                                    const SkyThresholdsLocal &th,
                                    cv::Mat *candidate,
                                    cv::Mat *seed,
                                    cv::Mat *cloud)
{
    if (!candidate || !seed || !cloud || small.empty()) return;
    const SkyFeatureLocal f = computeSkyFeatures(small);
    cv::Mat blur32;
    f.blur.convertTo(blur32, CV_32F);

    const int h = small.rows;
    const int w = small.cols;
    cv::Mat yy(h, w, CV_32F);
    for (int y = 0; y < h; ++y) yy.row(y).setTo((float)y);

    cv::Mat upperLimit = yy < (float)(h * 0.76);
    cv::Mat seedLimit = yy < (float)(h * 0.62);

    cv::Mat candMask =
        (blur32 >= th.brightCandidate) &
        (f.grad <= th.gradCandidate) &
        (f.texture <= th.textureCandidate) &
        (f.lap <= th.lapCandidate) &
        upperLimit;

    cv::Mat seedMask =
        (blur32 >= th.brightSeed) &
        (f.grad <= th.gradSeed) &
        (f.texture <= th.textureSeed) &
        seedLimit;

    cv::Mat highEdge = (f.grad > qMax(18.0, th.cloudGrad * 0.85)) |
                       (f.lap > qMax(8.0, th.cloudLap * 0.65));
    cv::Mat highEdge32;
    highEdge.convertTo(highEdge32, CV_32F, 1.0 / 255.0);
    cv::Mat edgeDensity;
    cv::blur(highEdge32, edgeDensity, cv::Size(17, 17));

    cv::Mat cloudLimit = yy < (float)(h * 0.78);
    cv::Mat cloudMask =
        (blur32 >= th.cloudBright) &
        (f.grad <= th.cloudGrad) &
        (f.texture <= th.cloudTexture) &
        (f.lap <= th.cloudLap) &
        (edgeDensity <= th.cloudEdgeDensity) &
        cloudLimit;

    cv::Mat openKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat closeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(7, 7));
    cv::Mat cloudCloseKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(13, 13));
    cv::morphologyEx(candMask, *candidate, cv::MORPH_OPEN, openKernel);
    cv::morphologyEx(*candidate, *candidate, cv::MORPH_CLOSE, closeKernel);
    cv::morphologyEx(seedMask, *seed, cv::MORPH_OPEN, openKernel);
    cv::morphologyEx(cloudMask, *cloud, cv::MORPH_OPEN, openKernel);
    cv::morphologyEx(*cloud, *cloud, cv::MORPH_CLOSE, cloudCloseKernel);
}

static cv::Mat keepSeededComponents(const cv::Mat &candidateIn, const cv::Mat &seed)
{
    cv::Mat candidate = cutNarrowChannels(candidateIn, 5);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(candidate, labels, stats, centroids, 8, CV_32S);
    cv::Mat out = cv::Mat::zeros(candidate.size(), CV_8U);
    const int minArea = qMax(120, (int)(candidate.total() * 0.00025));
    for (int lab = 1; lab < count; ++lab) {
        const int area = stats.at<int>(lab, cv::CC_STAT_AREA);
        if (area < minArea) continue;
        const int x = stats.at<int>(lab, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(lab, cv::CC_STAT_TOP);
        const int ww = stats.at<int>(lab, cv::CC_STAT_WIDTH);
        const int hh = stats.at<int>(lab, cv::CC_STAT_HEIGHT);
        cv::Mat comp = labels(cv::Rect(x, y, ww, hh)) == lab;
        cv::Mat seedRoi = seed(cv::Rect(x, y, ww, hh));
        cv::Mat seedHitsMask;
        cv::bitwise_and(seedRoi, comp, seedHitsMask);
        const int seedHits = cv::countNonZero(seedHitsMask);
        if (seedHits < 8) continue;

        std::vector<std::vector<cv::Point> > contours;
        cv::Mat compU8;
        comp.convertTo(compU8, CV_8U);
        cv::findContours(compU8, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        double perimeter = 0.0;
        for (size_t i = 0; i < contours.size(); ++i) perimeter += cv::arcLength(contours[i], true);
        const double raggedness = perimeter * perimeter / qMax(1.0, (double)area);
        if (area < minArea * 20 && raggedness > 95.0) continue;
        out.setTo(255, labels == lab);
    }
    return out;
}

static cv::Mat keepCloudComponents(const cv::Mat &cloudIn, const cv::Mat &clearMask, const cv::Mat &seed)
{
    cv::Mat cloudCandidate = cutNarrowChannels(cloudIn, 7);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(cloudCandidate, labels, stats, centroids, 8, CV_32S);
    cv::Mat out = cv::Mat::zeros(cloudCandidate.size(), CV_8U);
    const int minArea = qMax(300, (int)(cloudCandidate.total() * 0.00008));

    cv::Mat anchor;
    cv::bitwise_or(clearMask, seed, anchor);
    cv::Mat anchorKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(21, 21));
    cv::dilate(anchor, anchor, anchorKernel);

    for (int lab = 1; lab < count; ++lab) {
        const int area = stats.at<int>(lab, cv::CC_STAT_AREA);
        if (area < minArea) continue;
        const int x = stats.at<int>(lab, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(lab, cv::CC_STAT_TOP);
        const int ww = stats.at<int>(lab, cv::CC_STAT_WIDTH);
        const int hh = stats.at<int>(lab, cv::CC_STAT_HEIGHT);
        cv::Mat comp = labels(cv::Rect(x, y, ww, hh)) == lab;
        cv::Mat anchorRoi = anchor(cv::Rect(x, y, ww, hh));
        cv::Mat anchorHitsMask;
        cv::bitwise_and(anchorRoi, comp, anchorHitsMask);
        const int anchorHits = cv::countNonZero(anchorHitsMask);
        if (anchorHits < qMax(40, (int)(area * 0.06))) continue;

        std::vector<std::vector<cv::Point> > contours;
        cv::Mat compU8;
        comp.convertTo(compU8, CV_8U);
        cv::findContours(compU8, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        double perimeter = 0.0;
        for (size_t i = 0; i < contours.size(); ++i) perimeter += cv::arcLength(contours[i], true);
        const double raggedness = perimeter * perimeter / qMax(1.0, (double)area);
        if (area < minArea * 18 && raggedness > 105.0) continue;
        out.setTo(255, labels == lab);
    }
    return out;
}

static cv::Mat fillSmallHoles(const cv::Mat &mask, int maxArea)
{
    if (mask.empty()) return mask;
    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    cv::Mat inv;
    cv::bitwise_not(binary, inv);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(inv, labels, stats, centroids, 8, CV_32S);
    cv::Mat out = binary.clone();
    for (int lab = 1; lab < count; ++lab) {
        const int x = stats.at<int>(lab, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(lab, cv::CC_STAT_TOP);
        const int ww = stats.at<int>(lab, cv::CC_STAT_WIDTH);
        const int hh = stats.at<int>(lab, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(lab, cv::CC_STAT_AREA);
        const bool touchesBorder = x == 0 || y == 0 || x + ww >= mask.cols || y + hh >= mask.rows;
        if (!touchesBorder && area <= maxArea) out.setTo(255, labels == lab);
    }
    return out;
}

static cv::Mat pruneSkyFragments(const cv::Mat &mask)
{
    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    if (count <= 1) return binary;

    int largest = 0;
    int total = 0;
    for (int lab = 1; lab < count; ++lab) {
        const int area = stats.at<int>(lab, cv::CC_STAT_AREA);
        total += area;
        if (area > largest) largest = area;
    }
    const int minArea = qMax(qMax(9000, (int)(binary.total() * 0.0012)),
                             qMax((int)(largest * 0.04), (int)(total * 0.015)));
    cv::Mat out = cv::Mat::zeros(binary.size(), CV_8U);
    for (int lab = 1; lab < count; ++lab) {
        const int area = stats.at<int>(lab, cv::CC_STAT_AREA);
        const int x = stats.at<int>(lab, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(lab, cv::CC_STAT_TOP);
        const int ww = stats.at<int>(lab, cv::CC_STAT_WIDTH);
        const int hh = stats.at<int>(lab, cv::CC_STAT_HEIGHT);
        bool keep = (area == largest);
        if (!keep) {
            if (area < minArea) continue;
            if (ww < qMax(160, (int)(binary.cols * 0.018)) || hh < qMax(80, (int)(binary.rows * 0.035))) continue;
            cv::Mat comp = labels(cv::Rect(x, y, ww, hh)) == lab;
            std::vector<std::vector<cv::Point> > contours;
            cv::Mat compU8;
            comp.convertTo(compU8, CV_8U);
            cv::findContours(compU8, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            double perimeter = 0.0;
            for (size_t i = 0; i < contours.size(); ++i) perimeter += cv::arcLength(contours[i], true);
            const double raggedness = perimeter * perimeter / qMax(1.0, (double)area);
            const double fillRatio = (double)area / qMax(1.0, (double)(ww * hh));
            if (area < largest * 0.16 && (raggedness > 120.0 || fillRatio < 0.12)) continue;
            keep = true;
        }
        if (keep) out.setTo(255, labels == lab);
    }
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::morphologyEx(out, out, cv::MORPH_CLOSE, kernel);
    return out;
}

static cv::Mat buildTemporalSkyMaskPreparedSmallV1(const QVector<cv::Mat> &smalls)
{
    if (smalls.isEmpty() || smalls.first().empty()) return cv::Mat();
    const SkyThresholdsLocal th = computeSkyThresholds(smalls.first());
    QVector<cv::Mat> clearFrames;
    QVector<cv::Mat> cloudFrames;
    QVector<cv::Mat> skyFrames;
    for (int i = 0; i < smalls.size(); ++i) {
        cv::Mat candidate, seed, cloudCandidate;
        buildFrameSkyCandidates(smalls.at(i), th, &candidate, &seed, &cloudCandidate);
        cv::Mat clearMask = keepSeededComponents(candidate, seed);
        cv::Mat cloudMask = keepCloudComponents(cloudCandidate, clearMask, seed);
        cv::Mat skyMask;
        cv::bitwise_or(clearMask, cloudMask, skyMask);
        skyMask = fillSmallHoles(skyMask, 360);
        clearFrames.push_back(clearMask);
        cloudFrames.push_back(cloudMask);
        skyFrames.push_back(skyMask);
    }

    cv::Mat support = cv::Mat::zeros(skyFrames.first().size(), CV_8U);
    cv::Mat clearSupport = cv::Mat::zeros(skyFrames.first().size(), CV_8U);
    cv::Mat cloudSupport = cv::Mat::zeros(skyFrames.first().size(), CV_8U);
    cv::Mat sum = cv::Mat::zeros(skyFrames.first().size(), CV_32F);
    cv::Mat sumSq = cv::Mat::zeros(skyFrames.first().size(), CV_32F);
    for (int i = 0; i < smalls.size(); ++i) {
        support += (skyFrames.at(i) > 0) / 255;
        clearSupport += (clearFrames.at(i) > 0) / 255;
        cloudSupport += (cloudFrames.at(i) > 0) / 255;
        cv::Mat small32;
        smalls.at(i).convertTo(small32, CV_32F);
        sum += small32;
        sumSq += small32.mul(small32);
    }
    cv::Mat mean = sum / (double)smalls.size();
    cv::Mat variance = sumSq / (double)smalls.size() - mean.mul(mean);
    cv::max(variance, 0.0, variance);
    cv::Mat timeStd;
    cv::sqrt(variance, timeStd);

    cv::Mat clearSure = (clearFrames.first() > 0) & (clearSupport >= 2) & (timeStd <= 20.0);
    cv::Mat cloudSure = (cloudFrames.first() > 0) & (cloudSupport >= 2);
    cv::Mat sure;
    cv::bitwise_or(clearSure, cloudSure, sure);
    sure = fillSmallHoles(sure, 360);
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::morphologyEx(sure, sure, cv::MORPH_OPEN, kernel);
    cv::erode(sure, sure, kernel);
    sure = fillSmallHoles(sure, 360);
    sure = cutNarrowChannels(sure, 7);
    sure = pruneSkyFragments(sure);
    return sure;
}

struct SkyGeometryCleanupStatsLocal {
    int inputPixels = 0;
    int topConnectedPixels = 0;
    int neckAnchoredPixels = 0;
    int roughBoundaryPixels = 0;
    int outputPixels = 0;
};

static cv::Mat keepVerticalTopConnectedSky(const cv::Mat &mask)
{
    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    cv::Mat out = cv::Mat::zeros(binary.size(), CV_8U);
    cv::Mat active(1, binary.cols, CV_8U, cv::Scalar(255));
    for (int y = 0; y < binary.rows; ++y) {
        cv::bitwise_and(active, binary.row(y), active);
        active.copyTo(out.row(y));
    }
    return out;
}

static cv::Mat keepTopAnchoredComponents(const cv::Mat &mask)
{
    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    cv::Mat out = cv::Mat::zeros(binary.size(), CV_8U);
    const int minArea = qMax(500, (int)(binary.total() * 0.00002));
    for (int lab = 1; lab < count; ++lab) {
        const int area = stats.at<int>(lab, cv::CC_STAT_AREA);
        const int y = stats.at<int>(lab, cv::CC_STAT_TOP);
        if (area < minArea || y != 0) continue;
        out.setTo(255, labels == lab);
    }
    return out;
}

static cv::Mat cleanReplaySkyMaskGeometry(const cv::Mat &mask,
                                          SkyGeometryCleanupStatsLocal *stats)
{
    if (mask.empty()) return mask;

    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    SkyGeometryCleanupStatsLocal local;
    local.inputPixels = cv::countNonZero(binary);

    // Detection sky must remain continuously visible from the top of each
    // panorama column. Sideways pockets below foreground objects are excluded.
    cv::Mat topConnected = keepVerticalTopConnectedSky(binary);
    local.topConnectedPixels = cv::countNonZero(topConnected);
    if (local.topConnectedPixels < qMax(4096, local.inputPixels / 2)) {
        if (stats) {
            local.outputPixels = local.inputPixels;
            *stats = local;
        }
        return binary;
    }

    // A strong opening splits lobes joined to the main sky through a narrow
    // bottleneck. Only substantial pieces still anchored to the panorama top survive.
    const cv::Mat neckKernel =
        cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(21, 21));
    cv::Mat neckOpened;
    cv::morphologyEx(topConnected, neckOpened, cv::MORPH_OPEN, neckKernel);
    cv::Mat anchored = keepTopAnchoredComponents(neckOpened);
    local.neckAnchoredPixels = cv::countNonZero(anchored);
    if (local.neckAnchoredPixels < qMax(4096, local.inputPixels / 2)) {
        anchored = topConnected;
        local.neckAnchoredPixels = local.topConnectedPixels;
    }

    // A smooth contour has roughly one boundary pixel per local row/column.
    // Dense boundary neighborhoods indicate foliage-like or otherwise distorted
    // segments; those zones receive a substantially deeper local erosion.
    const cv::Mat edgeKernel =
        cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat inner;
    cv::erode(anchored, inner, edgeKernel);
    cv::Mat boundary;
    cv::subtract(anchored, inner, boundary);
    cv::Mat boundaryF;
    boundary.convertTo(boundaryF, CV_32F, 1.0 / 255.0);
    cv::Mat boundaryDensity;
    cv::boxFilter(boundaryF, boundaryDensity, CV_32F, cv::Size(41, 41),
                  cv::Point(-1, -1), true);

    cv::Mat roughBoundary = (boundary > 0) & (boundaryDensity >= 0.030);
    const int topGuardRows = qMin(anchored.rows, qMax(8, anchored.rows / 32));
    if (topGuardRows > 0) roughBoundary.rowRange(0, topGuardRows).setTo(0);
    local.roughBoundaryPixels = cv::countNonZero(roughBoundary);

    cv::Mat roughZone;
    const cv::Mat roughZoneKernel =
        cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(61, 61));
    cv::dilate(roughBoundary, roughZone, roughZoneKernel);

    cv::Mat deeplyEroded;
    const cv::Mat deepKernel =
        cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(25, 25));
    cv::erode(anchored, deeplyEroded, deepKernel);
    cv::Mat notDeeplyEroded;
    cv::bitwise_not(deeplyEroded, notDeeplyEroded);
    cv::Mat localRemoval;
    cv::bitwise_and(roughZone, notDeeplyEroded, localRemoval);

    cv::Mat cleaned = anchored.clone();
    cleaned.setTo(0, localRemoval);
    cleaned = keepVerticalTopConnectedSky(cleaned);
    local.outputPixels = cv::countNonZero(cleaned);
    if (local.outputPixels < qMax(4096, local.inputPixels / 2)) {
        cleaned = anchored;
        local.outputPixels = local.neckAnchoredPixels;
    }

    if (stats) *stats = local;
    return cleaned;
}

static cv::Mat shrinkSkyMaskSliceForDetection(const cv::Mat &smallSlice,
                                              int fullSliceWidth,
                                              int shrinkPixels)
{
    if (smallSlice.empty() || fullSliceWidth <= 0) return cv::Mat();
    cv::Mat detectionMask = smallSlice.clone();
    const int boundedShrink = qBound(0, shrinkPixels, 256);
    if (boundedShrink <= 0) return detectionMask;

    const double smallScale = (double)smallSlice.cols / (double)fullSliceWidth;
    const int smallShrink = qMax(1, (int)qRound(boundedShrink * smallScale));
    const cv::Mat edgeKernel = cv::getStructuringElement(
        cv::MORPH_ELLIPSE,
        cv::Size(smallShrink * 2 + 1, smallShrink * 2 + 1));
    cv::Mat eroded;
    cv::erode(smallSlice, eroded, edgeKernel);
    if (cv::countNonZero(eroded) > qMax(64, cv::countNonZero(smallSlice) / 5)) {
        detectionMask = eroded;
    }
    return detectionMask;
}

static cv::Mat buildDetectionSkyMaskSmall(const cv::Mat &skyMaskSmall,
                                          int segments,
                                          int fullSliceWidth,
                                          int shrinkPixels)
{
    if (skyMaskSmall.empty() || segments <= 0 || fullSliceWidth <= 0 ||
        skyMaskSmall.cols % segments != 0) {
        return cv::Mat();
    }

    const int smallSliceWidth = skyMaskSmall.cols / segments;
    cv::Mat detectionMask = cv::Mat::zeros(skyMaskSmall.size(), CV_8U);
    for (int tile = 0; tile < segments; ++tile) {
        const cv::Rect rect(tile * smallSliceWidth, 0, smallSliceWidth, skyMaskSmall.rows);
        const cv::Mat detectionSlice = shrinkSkyMaskSliceForDetection(
            skyMaskSmall(rect), fullSliceWidth, shrinkPixels);
        if (detectionSlice.empty()) return cv::Mat();
        detectionSlice.copyTo(detectionMask(rect));
    }
    return detectionMask;
}

static bool writeJsonObject(const QString &path, const QJsonObject &object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) {
        if (error) *error = QStringLiteral("cannot write JSON data: %1").arg(path);
        return false;
    }
    return true;
}

static bool archiveGeneratedSkyMasks(const QString &root,
                                     const QString &stream,
                                     const cv::Mat &rawMaskSmall,
                                     const cv::Mat &skyMaskSmall,
                                     const cv::Mat &detectionMaskSmall,
                                     qint64 generatedAtMs,
                                     quint64 round,
                                     int sourcePanoramas,
                                     int segments,
                                     int fullSliceWidth,
                                     int fullPanoramaWidth,
                                     int fullPanoramaHeight,
                                     int shrinkPixels,
                                     bool geometryCleanup,
                                     QString *archiveDirOut,
                                     QString *error)
{
    if (root.trimmed().isEmpty() || rawMaskSmall.empty() || skyMaskSmall.empty() ||
        detectionMaskSmall.empty() || rawMaskSmall.size() != skyMaskSmall.size() ||
        skyMaskSmall.size() != detectionMaskSmall.size()) {
        if (error) *error = QStringLiteral("invalid sky mask archive input");
        return false;
    }

    const QDateTime archivedAt = QDateTime::currentDateTime();
    const QDateTime generatedAt = generatedAtMs > 0
        ? QDateTime::fromMSecsSinceEpoch(generatedAtMs)
        : archivedAt;
    const QString archiveDir = QDir(root).filePath(
        generatedAt.toString(QStringLiteral("yyyyMMdd/HH")));
    const QString latestDir = QDir(root).filePath(QStringLiteral("latest"));
    if (!QDir().mkpath(archiveDir) || !QDir().mkpath(latestDir)) {
        if (error) *error = QStringLiteral("cannot create sky mask archive directories below %1").arg(root);
        return false;
    }

    const QString streamKey = stream.trimmed().toLower();
    const QString roundText = QStringLiteral("%1").arg((qulonglong)round, 4, 10, QLatin1Char('0'));
    const QString stem = QStringLiteral("%1_%2_round%3_pid%4")
        .arg(streamKey,
             generatedAt.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
             roundText,
             QString::number(QCoreApplication::applicationPid()));
    const QString rawPath = QDir(archiveDir).filePath(stem + QStringLiteral("_raw.png"));
    const QString skyPath = QDir(archiveDir).filePath(stem + QStringLiteral("_sky.png"));
    const QString detectionPath = QDir(archiveDir).filePath(stem + QStringLiteral("_detection.png"));
    const QString metadataPath = QDir(archiveDir).filePath(stem + QStringLiteral(".json"));

    if (!cv::imwrite(rawPath.toStdString(), rawMaskSmall) ||
        !cv::imwrite(skyPath.toStdString(), skyMaskSmall) ||
        !cv::imwrite(detectionPath.toStdString(), detectionMaskSmall)) {
        if (error) *error = QStringLiteral("one or more historical sky mask writes failed");
        return false;
    }

    const int rawPixels = cv::countNonZero(rawMaskSmall);
    const int skyPixels = cv::countNonZero(skyMaskSmall);
    const int detectionPixels = cv::countNonZero(detectionMaskSmall);
    const double totalPixels = (double)qMax<size_t>(1, skyMaskSmall.total());
    QJsonObject metadata;
    metadata.insert(QStringLiteral("generatedAt"),
                    generatedAt.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")));
    metadata.insert(QStringLiteral("archivedAt"),
                    archivedAt.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")));
    metadata.insert(QStringLiteral("stream"), stream.trimmed().toUpper());
    metadata.insert(QStringLiteral("generationMode"), QStringLiteral("first_complete_panoramas_once"));
    metadata.insert(QStringLiteral("archiveRule"), QStringLiteral("archive_after_entering_next_round"));
    metadata.insert(QStringLiteral("sourcePanoramas"), sourcePanoramas);
    metadata.insert(QStringLiteral("readyRound"), QString::number(round));
    metadata.insert(QStringLiteral("segments"), segments);
    metadata.insert(QStringLiteral("fullSliceWidth"), fullSliceWidth);
    metadata.insert(QStringLiteral("fullPanoramaWidth"), fullPanoramaWidth);
    metadata.insert(QStringLiteral("fullPanoramaHeight"), fullPanoramaHeight);
    metadata.insert(QStringLiteral("maskWidth"), skyMaskSmall.cols);
    metadata.insert(QStringLiteral("maskHeight"), skyMaskSmall.rows);
    metadata.insert(QStringLiteral("geometryCleanup"), geometryCleanup);
    metadata.insert(QStringLiteral("shrinkPixels"), qBound(0, shrinkPixels, 256));
    metadata.insert(QStringLiteral("rawMaskPixels"), rawPixels);
    metadata.insert(QStringLiteral("skyMaskPixels"), skyPixels);
    metadata.insert(QStringLiteral("detectionMaskPixels"), detectionPixels);
    metadata.insert(QStringLiteral("rawMaskRatio"), rawPixels / totalPixels);
    metadata.insert(QStringLiteral("skyMaskRatio"), skyPixels / totalPixels);
    metadata.insert(QStringLiteral("detectionMaskRatio"), detectionPixels / totalPixels);
    metadata.insert(QStringLiteral("rawMaskPath"), rawPath);
    metadata.insert(QStringLiteral("skyMaskPath"), skyPath);
    metadata.insert(QStringLiteral("detectionMaskPath"), detectionPath);

    QString jsonError;
    if (!writeJsonObject(metadataPath, metadata, &jsonError)) {
        if (error) *error = jsonError;
        return false;
    }

    const QString latestRaw = QDir(latestDir).filePath(streamKey + QStringLiteral("_raw.png"));
    const QString latestSky = QDir(latestDir).filePath(streamKey + QStringLiteral("_sky.png"));
    const QString latestDetection = QDir(latestDir).filePath(streamKey + QStringLiteral("_detection.png"));
    const QString latestMetadata = QDir(latestDir).filePath(streamKey + QStringLiteral(".json"));
    if (!cv::imwrite(latestRaw.toStdString(), rawMaskSmall) ||
        !cv::imwrite(latestSky.toStdString(), skyMaskSmall) ||
        !cv::imwrite(latestDetection.toStdString(), detectionMaskSmall) ||
        !writeJsonObject(latestMetadata, metadata, &jsonError)) {
        if (error) *error = jsonError.isEmpty()
            ? QStringLiteral("one or more latest sky mask writes failed")
            : jsonError;
        return false;
    }

    if (archiveDirOut) *archiveDirOut = archiveDir;
    return true;
}

static bool exportReplaySkyMaskPreview(const QString &outDir,
                                       const QString &stream,
                                       const cv::Mat &firstSmall,
                                       const cv::Mat &rawMaskSmall,
                                       const cv::Mat &geometryMaskSmall,
                                       int segments,
                                       int fullSliceW,
                                       int fullH,
                                       int shrinkPixels,
                                       QString *error)
{
    if (firstSmall.empty() || rawMaskSmall.empty() || geometryMaskSmall.empty() ||
        firstSmall.size() != rawMaskSmall.size() ||
        firstSmall.size() != geometryMaskSmall.size() ||
        segments <= 0 || geometryMaskSmall.cols % segments != 0 ||
        fullSliceW <= 0 || fullH <= 0) {
        if (error) *error = QStringLiteral("invalid panorama or mask dimensions");
        return false;
    }
    if (!QDir().mkpath(outDir)) {
        if (error) *error = QStringLiteral("cannot create output directory");
        return false;
    }

    const int smallSliceW = geometryMaskSmall.cols / segments;
    cv::Mat finalSmall = cv::Mat::zeros(geometryMaskSmall.size(), CV_8U);
    const int boundedShrink = qBound(0, shrinkPixels, 256);
    cv::Mat edgeKernel;
    if (boundedShrink > 0) {
        const int k = boundedShrink * 2 + 1;
        edgeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    }

    for (int tile = 0; tile < segments; ++tile) {
        const cv::Rect smallRect(tile * smallSliceW, 0, smallSliceW, geometryMaskSmall.rows);
        cv::Mat fullSlice;
        cv::resize(geometryMaskSmall(smallRect), fullSlice,
                   cv::Size(fullSliceW, fullH), 0, 0, cv::INTER_NEAREST);

        cv::Mat detectionMask = fullSlice;
        cv::Mat eroded;
        if (!edgeKernel.empty()) {
            cv::erode(fullSlice, eroded, edgeKernel);
            if (cv::countNonZero(eroded) > qMax(1024, cv::countNonZero(fullSlice) / 5)) {
                detectionMask = eroded;
            }
        }

        cv::Mat finalTileSmall;
        cv::resize(detectionMask, finalTileSmall,
                   cv::Size(smallSliceW, geometryMaskSmall.rows), 0, 0, cv::INTER_NEAREST);
        finalTileSmall.copyTo(finalSmall(smallRect));
    }

    cv::Mat firstGray;
    if (firstSmall.type() == CV_8U) firstGray = firstSmall;
    else firstSmall.convertTo(firstGray, CV_8U);

    cv::Mat overlay;
    cv::cvtColor(firstGray, overlay, cv::COLOR_GRAY2BGR);
    cv::Mat cyan(overlay.size(), overlay.type(), cv::Scalar(255, 210, 20));
    cv::Mat cyanBlend;
    cv::addWeighted(overlay, 0.52, cyan, 0.48, 0.0, cyanBlend);
    cyanBlend.copyTo(overlay, finalSmall);

    cv::Mat removed;
    cv::bitwise_and(rawMaskSmall, ~finalSmall, removed);
    cv::Mat red(overlay.size(), overlay.type(), cv::Scalar(30, 30, 255));
    cv::Mat redBlend;
    cv::addWeighted(overlay, 0.45, red, 0.55, 0.0, redBlend);
    redBlend.copyTo(overlay, removed);

    const QString prefix = stream.trimmed().toLower();
    const QString panoramaPath = QDir(outDir).filePath(prefix + QStringLiteral("_01_panorama_model_input.jpg"));
    const QString rawMaskPath = QDir(outDir).filePath(
        prefix + QStringLiteral("_02_mask_before_geometry_cleanup.png"));
    const QString geometryMaskPath = QDir(outDir).filePath(
        prefix + QStringLiteral("_03_mask_after_geometry_cleanup.png"));
    const QString finalMaskPath = QDir(outDir).filePath(
        prefix + QStringLiteral("_04_mask_after_shrink_%1px.png").arg(boundedShrink));
    const QString overlayPath = QDir(outDir).filePath(
        prefix + QStringLiteral("_05_overlay_cyan_keep_red_removed.jpg"));
    const QString overviewPath = QDir(outDir).filePath(
        prefix + QStringLiteral("_06_overlay_overview.jpg"));

    cv::Mat overview;
    cv::resize(overlay, overview, cv::Size(4096, qMax(1, overlay.rows * 4096 / overlay.cols)), 0, 0, cv::INTER_AREA);
    const std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, 94};
    if (!cv::imwrite(panoramaPath.toStdString(), firstGray, jpegParams) ||
        !cv::imwrite(rawMaskPath.toStdString(), rawMaskSmall) ||
        !cv::imwrite(geometryMaskPath.toStdString(), geometryMaskSmall) ||
        !cv::imwrite(finalMaskPath.toStdString(), finalSmall) ||
        !cv::imwrite(overlayPath.toStdString(), overlay, jpegParams) ||
        !cv::imwrite(overviewPath.toStdString(), overview, jpegParams)) {
        if (error) *error = QStringLiteral("one or more image writes failed");
        return false;
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("stream"), stream);
    manifest.insert(QStringLiteral("panoramaWidth"), firstGray.cols);
    manifest.insert(QStringLiteral("panoramaHeight"), firstGray.rows);
    manifest.insert(QStringLiteral("segments"), segments);
    manifest.insert(QStringLiteral("modelScale"), 0.25);
    manifest.insert(QStringLiteral("fullSliceWidth"), fullSliceW);
    manifest.insert(QStringLiteral("fullHeight"), fullH);
    manifest.insert(QStringLiteral("shrinkPixels"), boundedShrink);
    manifest.insert(QStringLiteral("baseMaskPixels"), cv::countNonZero(rawMaskSmall));
    manifest.insert(QStringLiteral("geometryMaskPixels"), cv::countNonZero(geometryMaskSmall));
    manifest.insert(QStringLiteral("finalMaskPixels"), cv::countNonZero(finalSmall));
    manifest.insert(QStringLiteral("baseMaskRatio"),
                    (double)cv::countNonZero(rawMaskSmall) / (double)rawMaskSmall.total());
    manifest.insert(QStringLiteral("geometryMaskRatio"),
                    (double)cv::countNonZero(geometryMaskSmall) /
                        (double)geometryMaskSmall.total());
    manifest.insert(QStringLiteral("finalMaskRatio"),
                    (double)cv::countNonZero(finalSmall) / (double)finalSmall.total());
    manifest.insert(QStringLiteral("overlayLegend"),
                    QStringLiteral("cyan=final detection sky, red=removed by geometry cleanup or shrink"));
    QFile manifestFile(QDir(outDir).filePath(QStringLiteral("manifest.json")));
    if (manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        manifestFile.close();
    }
    return true;
}

static cv::Mat buildTemporalSkyMaskV1(const QVector<cv::Mat> &frames)
{
    if (frames.isEmpty() || frames.first().empty()) return cv::Mat();
    QVector<cv::Mat> smalls;
    smalls.reserve(frames.size());
    for (int i = 0; i < frames.size(); ++i) {
        cv::Mat small;
        cv::resize(frames.at(i), small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
        smalls.push_back(small);
    }
    cv::Mat sure = buildTemporalSkyMaskPreparedSmallV1(smalls);
    if (sure.empty()) return cv::Mat();

    cv::Mat full;
    cv::resize(sure, full, frames.first().size(), 0, 0, cv::INTER_NEAREST);
    return full;
}

static bool updateBackgroundTile(VideoWorker::DetectBackgroundTile &tile,
                                 const cv::Mat &gray,
                                 int requiredFrames,
                                 int *readyHorizonY,
                                 bool buildTileSkyMask)
{
    if (gray.empty() || requiredFrames <= 0) return false;
    if (tile.ready) return true;

    cv::Mat f32;
    gray.convertTo(f32, CV_32F);
    if (tile.accum32f.empty()) tile.accum32f = cv::Mat::zeros(gray.size(), CV_32F);
    if (tile.accum32f.size() != gray.size()) {
        tile = VideoWorker::DetectBackgroundTile();
        tile.accum32f = cv::Mat::zeros(gray.size(), CV_32F);
    }

    tile.accum32f += f32;
    if (buildTileSkyMask) tile.initialFrames.push_back(gray.clone());
    ++tile.samples;
    if (tile.samples < requiredFrames) return false;

    cv::Mat avg = tile.accum32f / (double)tile.samples;
    avg.convertTo(tile.background8, CV_8U);
    tile.horizonY = estimateHorizonY(tile.background8);
    tile.horizonProfile = estimateHorizonProfile(tile.background8, tile.horizonY);
    if (buildTileSkyMask) {
        tile.skyMask = buildTemporalSkyMaskV1(tile.initialFrames);
    } else {
        tile.skyMask.release();
    }
    tile.initialFrames.clear();
    tile.accum32f.release();
    tile.ready = true;
    if (readyHorizonY) *readyHorizonY = tile.horizonY;
    return true;
}

static void updateBackgroundSlow(VideoWorker::DetectBackgroundTile &tile,
                                 const cv::Mat &gray,
                                 const cv::Mat &skyMask,
                                 const QVector<DetectCandidateLocal> &protectedTargets,
                                 double alpha,
                                 int interval,
                                 int protectRadius,
                                 int skyMargin)
{
    if (!tile.ready || tile.background8.empty() || gray.empty() || skyMask.empty()) return;
    if (alpha <= 0.0 || interval <= 0) return;
    ++tile.samples;
    if ((tile.samples % interval) != 0) return;

    cv::Mat updateMask = skyMask.clone();
    const int radius = qMax(0, protectRadius);
    if (radius > 0) {
        for (const DetectCandidateLocal &target : protectedTargets) {
            cv::circle(updateMask, cv::Point(target.x, target.y), radius, cv::Scalar(0), -1, cv::LINE_AA);
        }
    }
    if (cv::countNonZero(updateMask) <= 0) return;

    cv::Mat bg32;
    cv::Mat gray32;
    tile.background8.convertTo(bg32, CV_32F);
    gray.convertTo(gray32, CV_32F);
    cv::accumulateWeighted(gray32, bg32, alpha, updateMask);
    bg32.convertTo(tile.background8, CV_8U);
    tile.horizonY = estimateHorizonY(tile.background8);
    tile.horizonProfile = estimateHorizonProfile(tile.background8, tile.horizonY);
    Q_UNUSED(skyMargin);
}

static QImage fixedCropRgb32(const QImage &src, int cx, int cy, int size)
{
    QImage out(size, size, QImage::Format_RGB32);
    if (out.isNull()) return QImage();
    out.fill(Qt::black);
    if (src.isNull() || size <= 0) return out;

    const int half = size / 2;
    const QRect srcRect(0, 0, src.width(), src.height());
    const QRect wanted(cx - half, cy - half, size, size);
    const QRect copyRect = wanted.intersected(srcRect);
    if (copyRect.isEmpty()) return out;

    QPainter p(&out);
    p.drawImage(copyRect.topLeft() - wanted.topLeft(), src, copyRect);
    p.end();
    return out;
}

static bool parseFrameDateTime(const QString &sourcePath, QDateTime *dt)
{
    const QString stem = QFileInfo(sourcePath).completeBaseName();
    const QStringList parts = stem.split(QLatin1Char('_'));
    if (parts.size() < 3) return false;
    const QDate date = QDate::fromString(parts.at(1), QStringLiteral("yyyyMMdd"));
    const QTime time = QTime::fromString(parts.at(2).left(6), QStringLiteral("HHmmss"));
    if (!date.isValid() || !time.isValid()) return false;
    if (dt) *dt = QDateTime(date, time);
    return true;
}

static QString hourDirName(const QDateTime &dt)
{
    int hour = dt.time().hour();
    if (hour == 0) hour = 24;
    return QStringLiteral("%1").arg(hour, 2, 10, QLatin1Char('0'));
}

static QString candidateBaseName(const QString &sourcePath, int panoX, int panoY, int frameX, int frameY)
{
    QString stem = QFileInfo(sourcePath).completeBaseName().trimmed();
    if (stem.isEmpty()) stem = QStringLiteral("candidate");
    return QStringLiteral("%1_px%2_py%3_fx%4_fy%5")
        .arg(stem)
        .arg(panoX, 5, 10, QLatin1Char('0'))
        .arg(panoY, 5, 10, QLatin1Char('0'))
        .arg(frameX, 4, 10, QLatin1Char('0'))
        .arg(frameY, 4, 10, QLatin1Char('0'));
}

static QString uniqueCandidatePath(const QString &dir, const QString &base)
{
    QString path = QDir(dir).filePath(base + QStringLiteral(".jpg"));
    if (!QFileInfo(path).exists()) return path;
    for (int i = 1; i < 1000; ++i) {
        path = QDir(dir).filePath(base + QStringLiteral("_%1.jpg").arg(i, 3, 10, QLatin1Char('0')));
        if (!QFileInfo(path).exists()) return path;
    }
    return QDir(dir).filePath(base + QStringLiteral("_999.jpg"));
}

static void appendCandidateManifest(const QString &outDir, const QJsonObject &record)
{
    QFile f(QDir(outDir).filePath(QStringLiteral("manifest.jsonl")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    f.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
    f.write("\n");
}

static QString yoloClassName(const AppConfig &cfg, int classId)
{
    if (classId >= 0 && classId < cfg.detectYoloClassNames.size()) return cfg.detectYoloClassNames.at(classId);
    return classId >= 0 ? QStringLiteral("class_%1").arg(classId) : QString();
}

static QString yoloRunDiagnosticsText(const YoloRunDiagnosticsLocal &d)
{
    return QStringLiteral("shape=%1 rows=%2 pre=%3 nms=%4 rawmax=%5 cls=%6 wmax=%7 mxbox=%8,%9,%10,%11")
        .arg(d.outputShape.isEmpty() ? QStringLiteral("-") : d.outputShape)
        .arg(d.outputRows)
        .arg(d.preNmsCount)
        .arg(d.nmsCount)
        .arg(d.maxConfidence, 0, 'f', 7)
        .arg(d.maxClass)
        .arg(d.maxWeightedConfidence, 0, 'f', 7)
        .arg(d.maxX1)
        .arg(d.maxY1)
        .arg(d.maxX2)
        .arg(d.maxY2);
}

static cv::Mat qimageRgb32ToBgr(const QImage &img)
{
    if (img.isNull()) return cv::Mat();
    QImage rgb32 = img.format() == QImage::Format_RGB32 ? img : img.convertToFormat(QImage::Format_RGB32);
    cv::Mat bgra(rgb32.height(), rgb32.width(), CV_8UC4, const_cast<uchar*>(rgb32.constBits()), (size_t)rgb32.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr.clone();
}

static QVector<YoloDetectionLocal> runYoloOnBgr(cv::dnn::Net &net,
                                                const cv::Mat &srcBgr,
                                                int offsetX,
                                                int offsetY,
                                                const AppConfig &cfg,
                                                double confThreshold,
                                                YoloRunDiagnosticsLocal *diagnostics = nullptr)
{
    if (diagnostics) *diagnostics = YoloRunDiagnosticsLocal();
    QVector<YoloDetectionLocal> result;
    if (srcBgr.empty() || cfg.detectYoloInputSize <= 0) return result;

    const int input = cfg.detectYoloInputSize;
    cv::Mat resized;
    cv::resize(srcBgr, resized, cv::Size(input, input));
    cv::Mat blob = cv::dnn::blobFromImage(resized, 1.0 / 255.0, cv::Size(input, input), cv::Scalar(), true, false);

    std::vector<cv::Mat> outs;
    try {
        net.setInput(blob);
        net.forward(outs, net.getUnconnectedOutLayersNames());
    } catch (const cv::Exception &) {
        return result;
    }

    std::vector<cv::Rect> boxes;
    std::vector<float> scores;
    std::vector<int> classes;
    const int configuredClasses = cfg.detectYoloClassNames.size();
    auto isRawYoloAttrCount = [&](int attrs) {
        return configuredClasses > 0 &&
               (attrs == configuredClasses + 4 || attrs == configuredClasses + 5);
    };
    auto updateDiagnostics = [&](float conf, int classId, int left, int top, int right, int bottom) {
        if (!diagnostics) return;
        const double cropCx = (double)(left + right) * 0.5 - (double)offsetX;
        const double cropCy = (double)(top + bottom) * 0.5 - (double)offsetY;
        const double centerCx = (double)srcBgr.cols * 0.5;
        const double centerCy = (double)srcBgr.rows * 0.5;
        const double dist = std::sqrt((cropCx - centerCx) * (cropCx - centerCx) +
                                      (cropCy - centerCy) * (cropCy - centerCy));
        const double radius = qMax(1.0, cfg.detectYoloCenterWeightRadius);
        const double weighted = (double)conf * qMax(0.0, 1.0 - dist / radius);
        if ((double)conf > diagnostics->maxConfidence) {
            diagnostics->maxConfidence = conf;
            diagnostics->maxClass = classId;
            diagnostics->maxX1 = left;
            diagnostics->maxY1 = top;
            diagnostics->maxX2 = right;
            diagnostics->maxY2 = bottom;
        }
        diagnostics->maxWeightedConfidence = qMax(diagnostics->maxWeightedConfidence, weighted);
    };

    auto parseRow = [&](const float *row, int attrs, bool nmsFormat) {
        if (!row || attrs < 5) return;
        if (diagnostics) ++diagnostics->outputRows;
        if (nmsFormat && attrs >= 6) {
            const float conf = row[4];
            const int bestClass = qMax(0, (int)qRound(row[5]));
            float x1 = row[0];
            float y1 = row[1];
            float x2 = row[2];
            float y2 = row[3];
            const float maxCoord = qMax(qMax(qAbs(x1), qAbs(y1)), qMax(qAbs(x2), qAbs(y2)));
            if (maxCoord <= 2.0f) {
                x1 *= input;
                y1 *= input;
                x2 *= input;
                y2 *= input;
            }
            const double sx = (double)srcBgr.cols / (double)input;
            const double sy = (double)srcBgr.rows / (double)input;
            const int left = offsetX + (int)qRound(qMin(x1, x2) * sx);
            const int top = offsetY + (int)qRound(qMin(y1, y2) * sy);
            const int right = offsetX + (int)qRound(qMax(x1, x2) * sx);
            const int bottom = offsetY + (int)qRound(qMax(y1, y2) * sy);
            if (right <= left || bottom <= top) return;
            updateDiagnostics(conf, bestClass, left, top, right, bottom);
            if (conf < (float)confThreshold) return;
            boxes.push_back(cv::Rect(left, top, right - left, bottom - top));
            scores.push_back(conf);
            classes.push_back(bestClass);
            return;
        }
        const bool hasObjectness = (configuredClasses > 0 && attrs >= configuredClasses + 5);
        const int classStart = hasObjectness ? 5 : 4;
        const int availableClasses = attrs - classStart;
        if (availableClasses <= 0) return;
        const int classCount = configuredClasses > 0 ? qMin(configuredClasses, availableClasses) : availableClasses;

        int bestClass = 0;
        float bestClassScore = row[classStart];
        for (int c = 1; c < classCount; ++c) {
            const float v = row[classStart + c];
            if (v > bestClassScore) {
                bestClassScore = v;
                bestClass = c;
            }
        }
        const float objectness = hasObjectness ? row[4] : 1.0f;
        const float conf = objectness * bestClassScore;

        float cx = row[0];
        float cy = row[1];
        float bw = row[2];
        float bh = row[3];
        const float maxCoord = qMax(qMax(qAbs(cx), qAbs(cy)), qMax(qAbs(bw), qAbs(bh)));
        if (maxCoord <= 2.0f) {
            cx *= input;
            cy *= input;
            bw *= input;
            bh *= input;
        }
        if (bw <= 1.0f || bh <= 1.0f) return;

        const double sx = (double)srcBgr.cols / (double)input;
        const double sy = (double)srcBgr.rows / (double)input;
        int left = offsetX + (int)qRound((cx - bw * 0.5f) * sx);
        int top = offsetY + (int)qRound((cy - bh * 0.5f) * sy);
        int right = offsetX + (int)qRound((cx + bw * 0.5f) * sx);
        int bottom = offsetY + (int)qRound((cy + bh * 0.5f) * sy);
        if (right <= left || bottom <= top) return;
        updateDiagnostics(conf, bestClass, left, top, right, bottom);
        if (conf < (float)confThreshold) return;
        boxes.push_back(cv::Rect(left, top, right - left, bottom - top));
        scores.push_back(conf);
        classes.push_back(bestClass);
    };

    for (cv::Mat out : outs) {
        if (out.empty()) continue;
        if (out.type() != CV_32F) out.convertTo(out, CV_32F);
        if (diagnostics && diagnostics->outputShape.isEmpty()) {
            QStringList dims;
            for (int i = 0; i < out.dims; ++i) dims << QString::number(out.size[i]);
            diagnostics->outputShape = dims.join(QLatin1Char('x'));
        }
        if (out.dims == 3) {
            const int d1 = out.size[1];
            const int d2 = out.size[2];
            const float *base = reinterpret_cast<const float*>(out.data);
            if (d1 <= 256 && d2 > d1) {
                const bool nmsFormat = (d1 == 6 && !isRawYoloAttrCount(d1));
                QVector<float> row(d1);
                for (int i = 0; i < d2; ++i) {
                    for (int a = 0; a < d1; ++a) row[a] = base[a * d2 + i];
                    parseRow(row.constData(), d1, nmsFormat);
                }
            } else {
                for (int i = 0; i < d1; ++i) parseRow(base + i * d2, d2, d2 == 6 && d1 <= 1000);
            }
        } else if (out.dims == 2) {
            for (int i = 0; i < out.rows; ++i) parseRow(out.ptr<float>(i), out.cols, out.cols == 6 && out.rows <= 1000);
        }
    }

    std::vector<int> indices;
    if (diagnostics) diagnostics->preNmsCount = (int)boxes.size();
    if (!boxes.empty()) {
        cv::dnn::NMSBoxes(boxes, scores, (float)confThreshold, (float)cfg.detectYoloIouThreshold, indices);
    }
    if (diagnostics) diagnostics->nmsCount = (int)indices.size();
    for (int idx : indices) {
        YoloDetectionLocal d;
        d.x1 = boxes[idx].x;
        d.y1 = boxes[idx].y;
        d.x2 = boxes[idx].x + boxes[idx].width;
        d.y2 = boxes[idx].y + boxes[idx].height;
        d.classId = classes[idx];
        d.className = yoloClassName(cfg, d.classId);
        d.confidence = scores[idx];
        const double cropCx = (double)(d.x1 + d.x2) * 0.5 - (double)offsetX;
        const double cropCy = (double)(d.y1 + d.y2) * 0.5 - (double)offsetY;
        const double centerCx = (double)srcBgr.cols * 0.5;
        const double centerCy = (double)srcBgr.rows * 0.5;
        const double dist = std::sqrt((cropCx - centerCx) * (cropCx - centerCx) +
                                      (cropCy - centerCy) * (cropCy - centerCy));
        const double radius = qMax(1.0, cfg.detectYoloCenterWeightRadius);
        const double weight = qMax(0.0, 1.0 - dist / radius);
        d.weightedConfidence = d.confidence * weight;
        result.push_back(d);
    }
    if (diagnostics) diagnostics->resultCount = result.size();
    std::sort(result.begin(), result.end(), [](const YoloDetectionLocal &a, const YoloDetectionLocal &b) {
        if (qFuzzyCompare(a.weightedConfidence, b.weightedConfidence)) return a.confidence > b.confidence;
        return a.weightedConfidence > b.weightedConfidence;
    });
    return result;
}

static QVector<DetectCandidateLocal> selectedToCandidates(const QVector<SupplementWindowLocal> &wins)
{
    QVector<DetectCandidateLocal> out;
    out.reserve(wins.size());
    for (const SupplementWindowLocal &win : wins) {
        DetectCandidateLocal c;
        c.x = win.x;
        c.y = win.y;
        out.push_back(c);
    }
    return out;
}

static QVector<SupplementWindowLocal> chooseSupplementWindows(const cv::Mat &diff,
                                                              const cv::Mat &skyMask,
                                                              int windowSize,
                                                              int maxWindows,
                                                              double minSkyCoverage,
                                                              const QVector<DetectCandidateLocal> &existing,
                                                              int nmsRadius)
{
    QVector<SupplementWindowLocal> pool;
    if (diff.empty() || skyMask.empty() || windowSize <= 0 || maxWindows <= 0) return pool;
    const int w = diff.cols;
    const int h = diff.rows;
    const int half = windowSize / 2;
    if (w < windowSize || h < windowSize || half <= 0) return pool;
    const int step = qMax(64, windowSize / 2);
    for (int cy = half; cy + half <= h; cy += step) {
        for (int cx = half; cx + half <= w; cx += step) {
            DetectCandidateLocal probe;
            probe.x = cx;
            probe.y = cy;
            if (!candidateFarEnough(existing, probe, qMax(nmsRadius, half))) continue;
            const cv::Rect roi(cx - half, cy - half, windowSize, windowSize);
            const double coverage = cv::mean(skyMask(roi))[0] / 255.0;
            if (coverage < minSkyCoverage) continue;
            cv::Scalar mean, stddev;
            cv::meanStdDev(diff(roi), mean, stddev, skyMask(roi));
            SupplementWindowLocal win;
            win.x = cx;
            win.y = cy;
            win.size = windowSize;
            win.score = mean[0] + stddev[0] * 0.5;
            pool.push_back(win);
        }
    }
    std::sort(pool.begin(), pool.end(), [](const SupplementWindowLocal &a, const SupplementWindowLocal &b) {
        return a.score > b.score;
    });
    QVector<SupplementWindowLocal> selected;
    for (const SupplementWindowLocal &win : pool) {
        DetectCandidateLocal probe;
        probe.x = win.x;
        probe.y = win.y;
        if (!candidateFarEnough(selectedToCandidates(selected), probe, windowSize)) continue;
        selected.push_back(win);
        if (selected.size() >= maxWindows) break;
    }
    return selected;
}

static cv::Mat fixedCropGrayMat(const cv::Mat &gray, int cx, int cy, int size)
{
    cv::Mat out(size, size, gray.type(), cv::Scalar((gray.empty() ? 0 : cv::mean(gray)[0])));
    if (gray.empty() || size <= 0) return out;
    const int half = size / 2;
    const cv::Rect srcRect(0, 0, gray.cols, gray.rows);
    const cv::Rect wanted(cx - half, cy - half, size, size);
    const cv::Rect copyRect = wanted & srcRect;
    if (copyRect.empty()) return out;
    const int dx = copyRect.x - wanted.x;
    const int dy = copyRect.y - wanted.y;
    gray(copyRect).copyTo(out(cv::Rect(dx, dy, copyRect.width, copyRect.height)));
    return out;
}

static cv::Mat normalizedHighPassPatch(const cv::Mat &patch8)
{
    if (patch8.empty()) return cv::Mat();
    cv::Mat gray8 = patch8;
    if (gray8.type() != CV_8U) gray8.convertTo(gray8, CV_8U);
    cv::Mat eq;
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(gray8, eq);
    cv::Mat eq32;
    eq.convertTo(eq32, CV_32F);
    cv::Mat low;
    cv::GaussianBlur(eq32, low, cv::Size(0, 0), 7.0);
    cv::Mat hp = eq32 - low;
    hp -= cv::mean(hp)[0];
    const double norm = cv::norm(hp);
    if (norm > 1e-6) hp /= norm;
    return hp;
}

static double templateCorrelationAt(const cv::Mat &gray, const cv::Mat &templ, int cx, int cy)
{
    if (gray.empty() || templ.empty()) return 0.0;
    cv::Mat patch = fixedCropGrayMat(gray, cx, cy, templ.cols);
    cv::Mat hp = normalizedHighPassPatch(patch);
    if (hp.empty() || hp.size() != templ.size()) return 0.0;
    return hp.dot(templ);
}

static void pointDiffusionFeatures(const cv::Mat &gray,
                                   const cv::Mat &response,
                                   int cx,
                                   int cy,
                                   double *contrastOut,
                                   double *centerRingOut)
{
    const int size = 96;
    cv::Mat patch = fixedCropGrayMat(gray, cx, cy, size);
    cv::Mat resp = fixedCropGrayMat(response, cx, cy, size);
    double innerSum = 0.0;
    double ringSum = 0.0;
    double respInnerSum = 0.0;
    int innerCount = 0;
    int ringCount = 0;
    const double center = size / 2.0;
    for (int y = 0; y < size; ++y) {
        const uchar *p = patch.ptr<uchar>(y);
        const uchar *r = resp.ptr<uchar>(y);
        for (int x = 0; x < size; ++x) {
            const double dx = x - center;
            const double dy = y - center;
            const double inner = (dx * dx) / (13.0 * 13.0) + (dy * dy) / (8.0 * 8.0);
            const double outer = (dx * dx) / (36.0 * 36.0) + (dy * dy) / (24.0 * 24.0);
            if (inner <= 1.0) {
                innerSum += p[x];
                respInnerSum += r[x];
                ++innerCount;
            } else if (outer <= 1.0) {
                ringSum += p[x];
                ++ringCount;
            }
        }
    }
    const double innerMean = innerCount > 0 ? innerSum / innerCount : 0.0;
    const double ringMean = ringCount > 0 ? ringSum / ringCount : innerMean;
    const double respMean = innerCount > 0 ? respInnerSum / innerCount : 0.0;
    const double contrast = ringMean - innerMean;
    if (contrastOut) *contrastOut = contrast;
    if (centerRingOut) *centerRingOut = respMean * 2.0 + contrast;
}

static double localClutterScore(const cv::Mat &gray,
                                const cv::Mat &response,
                                int cx,
                                int cy)
{
    const int size = 128;
    cv::Mat patch = fixedCropGrayMat(gray, cx, cy, size);
    cv::Mat resp = fixedCropGrayMat(response, cx, cy, size);
    if (patch.empty() || resp.empty()) return 0.0;

    cv::Mat blur;
    cv::GaussianBlur(patch, blur, cv::Size(0, 0), 1.4);
    cv::Mat highpass;
    cv::absdiff(patch, blur, highpass);

    cv::Mat annulus = cv::Mat::zeros(patch.size(), CV_8U);
    const cv::Point center(size / 2, size / 2);
    cv::circle(annulus, center, 54, cv::Scalar(255), -1, cv::LINE_AA);
    cv::circle(annulus, center, 18, cv::Scalar(0), -1, cv::LINE_AA);

    cv::Scalar hpMean, hpStd;
    cv::meanStdDev(highpass, hpMean, hpStd, annulus);

    cv::Mat respEdges;
    cv::threshold(resp, respEdges, 12.0, 255.0, cv::THRESH_BINARY);
    cv::bitwise_and(respEdges, annulus, respEdges);
    const double annulusPixels = qMax(1, cv::countNonZero(annulus));
    const double responseDensity = (double)cv::countNonZero(respEdges) / annulusPixels;

    return hpMean[0] + hpStd[0] * 0.65 + responseDensity * 90.0;
}

bool VideoWorker::ensureYoloNet(const AppConfig &cfg)
{
    if (!cfg.detectYoloEnabled) return false;
    const QString path = cfg.detectYoloModelPath.trimmed();
    if (path.isEmpty()) return false;
    if (m_yoloModelPath != path) {
        m_yoloNet = cv::dnn::Net();
        m_yoloModelPath = path;
        m_yoloLoadAttempted = false;
        m_yoloReady = false;
    }
    if (m_yoloReady) return true;
    if (m_yoloLoadAttempted) return false;
    m_yoloLoadAttempted = true;

    if (!QFileInfo(path).isFile()) {
        emit logRequested(QStringLiteral("YOLO"), QStringLiteral("model not found: %1").arg(path), QStringLiteral("#F44336"));
        return false;
    }
    try {
        m_yoloNet = cv::dnn::readNetFromONNX(path.toStdString());
        m_yoloNet.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        m_yoloNet.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        m_yoloReady = !m_yoloNet.empty();
    } catch (const cv::Exception &e) {
        emit logRequested(QStringLiteral("YOLO"), QStringLiteral("load failed: %1").arg(QString::fromLocal8Bit(e.what())), QStringLiteral("#F44336"));
        m_yoloReady = false;
    }
    if (m_yoloReady) {
        emit logRequested(QStringLiteral("YOLO"), QStringLiteral("model ready: %1").arg(path), QStringLiteral("#6A9955"));
    }
    return m_yoloReady;
}

bool VideoWorker::ensureTraditionalTemplate(const AppConfig &cfg)
{
    const QString path = cfg.detectReferenceTemplatePath.trimmed();
    if (path.isEmpty()) {
        m_traditionalTemplate.release();
        m_traditionalTemplatePath.clear();
        m_traditionalTemplateLoadAttempted = false;
        return false;
    }
    if (m_traditionalTemplatePath != path) {
        m_traditionalTemplate.release();
        m_traditionalTemplatePath = path;
        m_traditionalTemplateLoadAttempted = false;
    }
    if (!m_traditionalTemplate.empty()) return true;
    if (m_traditionalTemplateLoadAttempted) return false;
    m_traditionalTemplateLoadAttempted = true;

    if (!QFileInfo(path).isFile()) {
        emit logRequested(QStringLiteral("DETECT"), QStringLiteral("traditional template not found: %1").arg(path), QStringLiteral("#FFD54F"));
        return false;
    }
    cv::Mat ref = cv::imread(path.toStdString(), cv::IMREAD_GRAYSCALE);
    if (ref.empty()) {
        emit logRequested(QStringLiteral("DETECT"), QStringLiteral("traditional template decode failed: %1").arg(path), QStringLiteral("#FFD54F"));
        return false;
    }
    const int size = 96;
    const int cx = ref.cols / 2;
    const int cy = ref.rows / 2;
    m_traditionalTemplate = normalizedHighPassPatch(fixedCropGrayMat(ref, cx, cy, size));
    if (m_traditionalTemplate.empty()) return false;
    emit logRequested(QStringLiteral("DETECT"), QStringLiteral("traditional template ready: %1").arg(path), QStringLiteral("#6A9955"));
    return true;
}

static int bitArrayTrueCountLocal(const QBitArray &bits)
{
    int n = 0;
    for (int i = 0; i < bits.size(); ++i) {
        if (bits.testBit(i)) ++n;
    }
    return n;
}

static DetectSkyMaskBuildResultLocal buildPanoramaSkyMaskInBackground(
    const QVector<cv::Mat> &smallFrames,
    const QString &stream,
    bool geometryCleanup,
    bool directYoloEnabled,
    const QString &dataRoot,
    qint64 applicationPid,
    bool replayMode,
    const QString &previewDir,
    int segments,
    int fullSliceWidth,
    int fullPanoramaHeight,
    int shrinkPixels)
{
    DetectSkyMaskBuildResultLocal result;
    QElapsedTimer timer;
    timer.start();
    try {
        result.rawMaskSmall = buildTemporalSkyMaskPreparedSmallV1(smallFrames);
        result.maskSmall = result.rawMaskSmall;
        result.geometryCleanupApplied = geometryCleanup;
        if (geometryCleanup && !result.maskSmall.empty()) {
            SkyGeometryCleanupStatsLocal stats;
            result.maskSmall = cleanReplaySkyMaskGeometry(result.maskSmall, &stats);
            result.cleanupInputPixels = stats.inputPixels;
            result.cleanupTopConnectedPixels = stats.topConnectedPixels;
            result.cleanupNeckAnchoredPixels = stats.neckAnchoredPixels;
            result.cleanupRoughBoundaryPixels = stats.roughBoundaryPixels;
            result.cleanupOutputPixels = stats.outputPixels;
        }
        result.ready = !result.maskSmall.empty();
        if (result.ready) result.generatedAtMs = QDateTime::currentMSecsSinceEpoch();

#ifdef DMX_ADVANCED_DETECTION
        if (result.ready && directYoloEnabled) {
            result.runtimeMaskWriteAttempted = true;
            const QString root = dataRoot.trimmed().isEmpty()
                ? QDir::tempPath()
                : QDir(dataRoot).filePath(QStringLiteral("runtime/direct_yolo_masks"));
            if (!QDir().mkpath(root)) {
                result.runtimeMaskError = QStringLiteral("cannot create runtime mask directory: %1").arg(root);
            } else {
                result.runtimeMaskPath = QDir(root).filePath(
                    QStringLiteral("%1_%2.png")
                        .arg(stream.toLower())
                        .arg(applicationPid));
                if (!cv::imwrite(result.runtimeMaskPath.toStdString(), result.maskSmall)) {
                    result.runtimeMaskPath.clear();
                    result.runtimeMaskError = QStringLiteral("runtime sky mask write failed");
                }
            }
        }
#else
        Q_UNUSED(directYoloEnabled)
        Q_UNUSED(dataRoot)
        Q_UNUSED(applicationPid)
#endif

        if (replayMode && result.ready && !previewDir.trimmed().isEmpty() &&
            !smallFrames.isEmpty()) {
            result.previewAttempted = true;
            result.previewDir = previewDir;
            result.previewExported = exportReplaySkyMaskPreview(
                previewDir,
                stream,
                smallFrames.first(),
                result.rawMaskSmall,
                result.maskSmall,
                segments,
                fullSliceWidth,
                fullPanoramaHeight,
                shrinkPixels,
                &result.previewError);
        }
    } catch (const cv::Exception &e) {
        result.error = QString::fromLocal8Bit(e.what());
        result.ready = false;
    } catch (const std::exception &e) {
        result.error = QString::fromLocal8Bit(e.what());
        result.ready = false;
    } catch (...) {
        result.error = QStringLiteral("unknown background mask build error");
        result.ready = false;
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

static DetectSkyMaskArchiveResultLocal archivePanoramaSkyMaskInBackground(
    const QString &archiveRoot,
    const QString &stream,
    const cv::Mat &rawMaskSmall,
    const cv::Mat &maskSmall,
    qint64 generatedAtMs,
    quint64 readyRound,
    int sourcePanoramas,
    int segments,
    int fullSliceWidth,
    int fullPanoramaWidth,
    int fullPanoramaHeight,
    int shrinkPixels,
    bool geometryCleanup)
{
    DetectSkyMaskArchiveResultLocal result;
    QElapsedTimer timer;
    timer.start();
    try {
        const cv::Mat detectionMaskSmall = buildDetectionSkyMaskSmall(
            maskSmall, segments, fullSliceWidth, shrinkPixels);
        result.archived = archiveGeneratedSkyMasks(
            archiveRoot,
            stream,
            rawMaskSmall,
            maskSmall,
            detectionMaskSmall,
            generatedAtMs,
            readyRound,
            sourcePanoramas,
            segments,
            fullSliceWidth,
            fullPanoramaWidth,
            fullPanoramaHeight,
            shrinkPixels,
            geometryCleanup,
            &result.archiveDir,
            &result.error);
    } catch (const cv::Exception &e) {
        result.error = QString::fromLocal8Bit(e.what());
    } catch (const std::exception &e) {
        result.error = QString::fromLocal8Bit(e.what());
    } catch (...) {
        result.error = QStringLiteral("unknown background mask archive error");
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

static void resetPanoramaSkyMaskSamples(DetectSkyMaskPanoramaStateLocal &state)
{
    state.smallFrames.clear();
    state.seenTiles.clear();
    state.smallFrames.reserve(state.required);
    state.seenTiles.reserve(state.required);
    for (int i = 0; i < state.required; ++i) {
        state.smallFrames.push_back(cv::Mat::zeros(state.smallH, state.smallW, CV_8U));
        state.seenTiles.push_back(QBitArray(state.segments, false));
    }
}

cv::Mat VideoWorker::updateAndGetPanoramaSkyMaskSlice(const QString &stream,
                                                      const cv::Mat &gray,
                                                      int tileIndex,
                                                      int segments,
                                                      int sliceW,
                                                      int panoW,
                                                      int panoH,
                                                      quint64 totalFrames,
                                                      const AppConfig &cfg)
{
    if (gray.empty() || segments <= 0 || sliceW <= 0 || panoW <= 0 || panoH <= 0 || totalFrames == 0) {
        return cv::Mat();
    }
    if (tileIndex < 0 || tileIndex >= segments) return cv::Mat();

    const QString s = (stream == QStringLiteral("GRAY")) ? QStringLiteral("BW") : stream.trimmed().toUpper();
    DetectSkyMaskPanoramaStateLocal &state = (s == QStringLiteral("BW")) ? m_skyMaskPanoramaBw : m_skyMaskPanoramaRgb;
    const int required = qMax(1, cfg.detectBackgroundFrames);
    const int smallSliceW = qMax(1, sliceW / 4);
    const int smallH = qMax(64, panoH / 4);
    const int smallW = smallSliceW * segments;

    const bool needInit =
        state.required != required ||
        state.segments != segments ||
        state.sliceW != sliceW ||
        state.panoW != panoW ||
        state.panoH != panoH ||
        state.smallW != smallW ||
        state.smallH != smallH ||
        state.smallSliceW != smallSliceW;
    if (needInit) {
        state = DetectSkyMaskPanoramaStateLocal();
        state.required = required;
        state.segments = segments;
        state.sliceW = sliceW;
        state.panoW = panoW;
        state.panoH = panoH;
        state.smallW = smallW;
        state.smallH = smallH;
        state.smallSliceW = smallSliceW;
        resetPanoramaSkyMaskSamples(state);
        emit logRequested(QStringLiteral("DETECT"),
            QStringLiteral("%1 panorama sky mask init: samples=%2 segments=%3 small=%4x%5")
                .arg(s).arg(required).arg(segments).arg(smallW).arg(smallH),
            QStringLiteral("#569CD6"));
#ifdef DMX_ADVANCED_DETECTION
        const char *preloadVariable =
            (s == QStringLiteral("BW")) ? "DMX_TEST_SKY_MASK_BW" : "DMX_TEST_SKY_MASK_RGB";
        const QString preloadPath = qEnvironmentVariable(preloadVariable).trimmed();
        if (!preloadPath.isEmpty()) {
            const cv::Mat preloaded = cv::imread(preloadPath.toStdString(), cv::IMREAD_GRAYSCALE);
            if (!preloaded.empty() && preloaded.cols == smallW && preloaded.rows == smallH) {
                state.maskSmall = preloaded;
                state.ready = true;
                state.readyRound = 0;
                state.runtimeMaskPath = preloadPath;
                state.smallFrames.clear();
                state.seenTiles.clear();
                emit logRequested(
                    QStringLiteral("DETECT"),
                    QStringLiteral("%1 panorama sky mask preloaded: %2").arg(s, preloadPath),
                    QStringLiteral("#6A9955"));
            } else {
                emit logRequested(
                    QStringLiteral("DETECT"),
                    QStringLiteral("%1 panorama sky mask preload rejected: %2 expected=%3x%4")
                        .arg(s, preloadPath)
                        .arg(smallW)
                        .arg(smallH),
                    QStringLiteral("#F44336"));
            }
        }
#endif
    }

    const quint64 round = ((totalFrames - 1) / (quint64)segments) + 1;

    if (!state.ready && state.buildFuture &&
        state.buildFuture->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const DetectSkyMaskBuildResultLocal result = state.buildFuture->get();
        state.buildFuture.reset();
        if (result.ready) {
            state.ready = true;
            state.readyRound = state.buildRound;
            state.rawMaskSmall = result.rawMaskSmall;
            state.maskSmall = result.maskSmall;
            state.geometryCleanupApplied = result.geometryCleanupApplied;
            state.generatedAtMs = result.generatedAtMs;
#ifdef DMX_ADVANCED_DETECTION
            state.runtimeMaskPath = result.runtimeMaskPath;
#endif
            if (result.geometryCleanupApplied) {
                emit logRequested(QStringLiteral("DETECT"),
                    QStringLiteral("%1 panorama sky geometry cleanup: "
                                   "raw=%2 top=%3 neck=%4 rough=%5 final=%6")
                        .arg(s)
                        .arg(result.cleanupInputPixels)
                        .arg(result.cleanupTopConnectedPixels)
                        .arg(result.cleanupNeckAnchoredPixels)
                        .arg(result.cleanupRoughBoundaryPixels)
                        .arg(result.cleanupOutputPixels),
                    QStringLiteral("#FFD54F"));
            }
#ifdef DMX_ADVANCED_DETECTION
            if (result.runtimeMaskWriteAttempted) {
                emit logRequested(
                    QStringLiteral("YOLO_DIRECT"),
                    result.runtimeMaskPath.isEmpty()
                        ? QStringLiteral("%1 runtime sky mask write failed: %2")
                              .arg(s, result.runtimeMaskError)
                        : QStringLiteral("%1 runtime sky mask ready: %2")
                              .arg(s, result.runtimeMaskPath),
                    result.runtimeMaskPath.isEmpty()
                        ? QStringLiteral("#F44336")
                        : QStringLiteral("#6A9955"));
            }
#endif
            if (result.previewAttempted) {
                emit logRequested(QStringLiteral("DETECT"),
                    result.previewExported
                        ? QStringLiteral("%1 panorama sky mask preview exported: %2")
                              .arg(s, result.previewDir)
                        : QStringLiteral("%1 panorama sky mask preview export failed: %2")
                              .arg(s, result.previewError),
                    result.previewExported
                        ? QStringLiteral("#6A9955")
                        : QStringLiteral("#F44336"));
            }
            emit logRequested(QStringLiteral("DETECT"),
                QStringLiteral("%1 panorama sky mask ready in background: round=%2 small=%3x%4 elapsed=%5ms")
                    .arg(s)
                    .arg((qulonglong)state.readyRound)
                    .arg(state.maskSmall.cols)
                    .arg(state.maskSmall.rows)
                    .arg(result.elapsedMs),
                QStringLiteral("#6A9955"));
        } else {
            const QString error = result.error.trimmed().isEmpty()
                ? QStringLiteral("empty mask result")
                : result.error;
            state.buildStarted = false;
            state.buildRound = 0;
            resetPanoramaSkyMaskSamples(state);
            emit logRequested(QStringLiteral("DETECT"),
                QStringLiteral("%1 panorama sky mask background build failed after %2ms: %3; collecting new samples")
                    .arg(s)
                    .arg(result.elapsedMs)
                    .arg(error),
                QStringLiteral("#F44336"));
        }
    }

    if (!state.ready && !state.buildStarted) {
        int sampleIdx = -1;
        for (int i = 0; i < state.seenTiles.size(); ++i) {
            if (!state.seenTiles[i].testBit(tileIndex)) {
                sampleIdx = i;
                break;
            }
        }
        if (sampleIdx >= 0 && sampleIdx < state.smallFrames.size()) {
            cv::Mat smallTile;
            cv::resize(gray, smallTile, cv::Size(smallSliceW, smallH), 0, 0, cv::INTER_AREA);
            const int x0 = tileIndex * smallSliceW;
            if (x0 >= 0 && x0 + smallSliceW <= state.smallFrames[sampleIdx].cols) {
                smallTile.copyTo(state.smallFrames[sampleIdx](cv::Rect(x0, 0, smallSliceW, smallH)));
                state.seenTiles[sampleIdx].setBit(tileIndex, true);
            }
        }

        bool complete = true;
        for (int i = 0; i < state.seenTiles.size(); ++i) {
            if (bitArrayTrueCountLocal(state.seenTiles[i]) < segments) {
                complete = false;
                break;
            }
        }
        if (complete) {
            const bool geometryCleanup =
                qEnvironmentVariableIntValue("DMX_DETECT_SKY_GEOMETRY_CLEANUP") == 1;
#ifdef DMX_ADVANCED_DETECTION
            const bool directYoloEnabled = qEnvironmentVariableIntValue("DMX_DIRECT_YOLO") == 1;
#else
            const bool directYoloEnabled = false;
#endif
            const QString exportDir = qEnvironmentVariable("DMX_SKY_MASK_EXPORT_DIR").trimmed();
            QVector<cv::Mat> buildFrames;
            buildFrames.swap(state.smallFrames);
            state.smallFrames.clear();
            state.seenTiles.clear();
            state.buildStarted = true;
            state.buildRound = round;
            const QString dataRoot = qEnvironmentVariable("DMX_DATA_ROOT").trimmed();
            const qint64 applicationPid = QCoreApplication::applicationPid();
            const bool replayMode = cfg.replayMode;
            const int shrinkPixels = cfg.detectSkyShrinkPixels;
            state.buildFuture = std::make_shared<std::future<DetectSkyMaskBuildResultLocal>>(
                std::async(std::launch::async,
                    [buildFrames, s, geometryCleanup, directYoloEnabled, dataRoot,
                     applicationPid, replayMode, exportDir, segments, sliceW,
                     panoH, shrinkPixels]() {
                        return buildPanoramaSkyMaskInBackground(
                            buildFrames,
                            s,
                            geometryCleanup,
                            directYoloEnabled,
                            dataRoot,
                            applicationPid,
                            replayMode,
                            exportDir,
                            segments,
                            sliceW,
                            panoH,
                            shrinkPixels);
                    }));
            emit logRequested(QStringLiteral("DETECT"),
                QStringLiteral("%1 panorama sky mask background build started: round=%2 samples=%3 small=%4x%5")
                    .arg(s)
                    .arg((qulonglong)round)
                    .arg(required)
                    .arg(smallW)
                    .arg(smallH),
                QStringLiteral("#569CD6"));
        }
    }

    if (!state.ready || state.maskSmall.empty() || round <= state.readyRound) return cv::Mat();

    if (state.archiveFuture &&
        state.archiveFuture->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const DetectSkyMaskArchiveResultLocal result = state.archiveFuture->get();
        state.archiveFuture.reset();
        state.archiveAttempted = true;
        emit logRequested(
            QStringLiteral("DETECT"),
            result.archived
                ? QStringLiteral("%1 panorama sky masks archived in background: %2 elapsed=%3ms")
                      .arg(s, result.archiveDir)
                      .arg(result.elapsedMs)
                : QStringLiteral("%1 panorama sky mask background archive failed after %2ms: %3")
                      .arg(s)
                      .arg(result.elapsedMs)
                      .arg(result.error),
            result.archived ? QStringLiteral("#6A9955") : QStringLiteral("#F44336"));
    }
    if (!state.archiveStarted && !state.archiveAttempted) {
        state.archiveStarted = true;
        if (!state.rawMaskSmall.empty() && !cfg.detectSkyMaskSaveRoot.trimmed().isEmpty()) {
            const QString archiveRoot = cfg.detectSkyMaskSaveRoot;
            const cv::Mat rawMaskForArchive = state.rawMaskSmall;
            const cv::Mat maskForArchive = state.maskSmall;
            const qint64 generatedAtMs = state.generatedAtMs;
            const quint64 readyRound = state.readyRound;
            const int shrinkPixels = cfg.detectSkyShrinkPixels;
            const bool geometryCleanup = state.geometryCleanupApplied;
            state.archiveFuture = std::make_shared<std::future<DetectSkyMaskArchiveResultLocal>>(
                std::async(std::launch::async,
                    [archiveRoot, s, rawMaskForArchive, maskForArchive, generatedAtMs,
                     readyRound, required, segments, sliceW, panoW, panoH,
                     shrinkPixels, geometryCleanup]() {
                        return archivePanoramaSkyMaskInBackground(
                            archiveRoot,
                            s,
                            rawMaskForArchive,
                            maskForArchive,
                            generatedAtMs,
                            readyRound,
                            required,
                            segments,
                            sliceW,
                            panoW,
                            panoH,
                            shrinkPixels,
                            geometryCleanup);
                    }));
            emit logRequested(QStringLiteral("DETECT"),
                QStringLiteral("%1 panorama sky mask background archive started after entering round %2")
                    .arg(s)
                    .arg((qulonglong)round),
                QStringLiteral("#569CD6"));
        } else {
            state.archiveAttempted = true;
        }
        state.rawMaskSmall.release();
    }
    if (state.maskSmall.cols < smallW || state.maskSmall.rows < smallH) return cv::Mat();

    const int x0 = tileIndex * smallSliceW;
    if (x0 < 0 || x0 + smallSliceW > state.maskSmall.cols) return cv::Mat();
    cv::Mat smallSlice = state.maskSmall(cv::Rect(x0, 0, smallSliceW, smallH)).clone();
#ifdef DMX_ADVANCED_DETECTION
    const cv::Mat detectionSlice = shrinkSkyMaskSliceForDetection(
        smallSlice, sliceW, cfg.detectSkyShrinkPixels);
    if (!detectionSlice.empty()) smallSlice = detectionSlice;
#endif
    cv::Mat fullSlice;
    cv::resize(smallSlice, fullSlice, gray.size(), 0, 0, cv::INTER_NEAREST);
    if (!state.readyLogged) {
        state.readyLogged = true;
        emit logRequested(QStringLiteral("DETECT"),
            QStringLiteral("%1 using panorama sky mask slices from round>%2 shrink=%3px")
                .arg(s)
                .arg((qulonglong)state.readyRound)
                .arg(cfg.detectSkyShrinkPixels),
            QStringLiteral("#6A9955"));
    }
    return fullSlice;
}

void VideoWorker::maybeFinalizeRoundCandidates(const QString &stream, quint64 fileIdx, qint64 rxMs, const QString &sourcePath)
{
    if (!m_cache) return;
    const AppConfig &cfg = AppConfig::instance();
    if (!cfg.detectEnabled) return;

    const QString s = (stream == QStringLiteral("GRAY")) ? QStringLiteral("BW") : stream.trimmed().toUpper();
    const QString detectStream = cfg.detectStream.trimmed().toUpper();
    if (detectStream != QStringLiteral("BOTH") && detectStream != s) return;

    const PanoramaCache::BlockState st = (s == QStringLiteral("BW"))
        ? m_cache->state(PanoramaCache::FullBw)
        : m_cache->state(PanoramaCache::FullRgb);
    if (!st.inited || !st.hasWrapped || st.segments <= 0 || st.panoW <= 0 || st.panoH <= 0) return;
    if ((st.totalFrames % (quint64)qMax(1, st.segments)) != 0) return;

    const quint64 round = st.totalFrames / (quint64)qMax(1, st.segments);
    quint64 &lastRound = (s == QStringLiteral("BW")) ? m_lastFinalizedRoundBw : m_lastFinalizedRoundRgb;
    if (round == 0 || round <= lastRound) return;
    lastRound = round;

    finalizeRoundCandidates(s, round, fileIdx, rxMs, sourcePath);
}

void VideoWorker::collectTileCandidates(const QString &stream, const QImage &frameRgb32, double angleDeg, quint64 fileIdx, qint64 rxMs, const QString &sourcePath)
{
    const AppConfig &cfg = AppConfig::instance();
    if (!cfg.detectEnabled || frameRgb32.isNull()) return;
    const QString detectStream = cfg.detectStream.trimmed().toUpper();
    const QString s = (stream == QStringLiteral("GRAY")) ? QStringLiteral("BW") : stream.trimmed().toUpper();
    const bool traditionalStreamEnabled =
        detectStream == QStringLiteral("BOTH") || detectStream == s;
#ifdef DMX_ADVANCED_DETECTION
    const bool directYoloEnabled = qEnvironmentVariableIntValue("DMX_DIRECT_YOLO") == 1;
#else
    const bool directYoloEnabled = false;
#endif
    if (!traditionalStreamEnabled && !directYoloEnabled) return;
    if (cfg.detectSaveRoot.trimmed().isEmpty()) return;

    int w = frameRgb32.width();
    int h = frameRgb32.height();
    if (w <= 0 || h <= 0) return;

    cv::Mat bgra(h, w, CV_8UC4, const_cast<uchar*>(frameRgb32.constBits()), (size_t)frameRgb32.bytesPerLine());
    cv::Mat gray;
    cv::cvtColor(bgra, gray, cv::COLOR_BGRA2GRAY);

    if (!directYoloEnabled && cfg.detectMedianKernel > 1) {
        const int k = oddKernel(cfg.detectMedianKernel, 3);
        cv::medianBlur(gray, gray, k);
    }

    const int segments = qMax(1, cfg.fullWidth / qMax(1, w));
    int tileIndex = 0;
    if (angleDeg >= 0.0) {
        const double storeAngle = normalizeAngleLocal(angleDeg);
        tileIndex = (int)(storeAngle / 360.0 * segments) % segments;
    } else {
        tileIndex = (fileIdx > 0) ? (int)(fileIdx % (quint64)segments) : 0;
    }
    if (s == QStringLiteral("BW")) tileIndex = (tileIndex + segments / 2) % segments;
    if (tileIndex < 0) tileIndex = 0;
    const int sliceW = qMax(1, cfg.fullWidth / segments);
    if (!m_cache) return;
    const PanoramaCache::BlockState stForMask = (s == QStringLiteral("BW"))
        ? m_cache->state(PanoramaCache::FullBw)
        : m_cache->state(PanoramaCache::FullRgb);
    const int maskSegments = stForMask.segments > 0 ? stForMask.segments : segments;
    const int maskSliceW = stForMask.sliceW > 0 ? stForMask.sliceW : sliceW;
    const int maskPanoW = stForMask.panoW > 0 ? stForMask.panoW : cfg.fullWidth;
    const int maskPanoH = stForMask.panoH > 0 ? stForMask.panoH : cfg.fullHeight;
    cv::Mat panoramaSkyMask = updateAndGetPanoramaSkyMaskSlice(
        s, gray, tileIndex, maskSegments, maskSliceW, maskPanoW, maskPanoH, stForMask.totalFrames, cfg);
    const bool detectionReady = !panoramaSkyMask.empty();

#ifdef DMX_ADVANCED_DETECTION
    if (directYoloEnabled && detectionReady) {
        const DetectSkyMaskPanoramaStateLocal &directMaskState =
            (s == QStringLiteral("BW")) ? m_skyMaskPanoramaBw : m_skyMaskPanoramaRgb;
        emit directYoloFrameReady(
            s,
            sourcePath,
            angleDeg,
            fileIdx,
            rxMs,
            tileIndex,
            maskSegments,
            maskSliceW,
            maskPanoW,
            maskPanoH,
            directMaskState.ready ? directMaskState.runtimeMaskPath : QString(),
            cfg.detectSkyShrinkPixels);
    }
#endif

    // The first complete panoramas are reserved for sky-mask modeling. Start
    // every detection path only after the generated mask is active next round.
    if (!detectionReady) return;
    if (!traditionalStreamEnabled) return;

#ifdef DMX_ADVANCED_DETECTION
    const int traditionalDownscale = qBound(
        1,
        qEnvironmentVariableIntValue("DMX_TRADITIONAL_DOWNSCALE"),
        4);
    if (directYoloEnabled && traditionalDownscale > 1) {
        cv::Mat scaledGray;
        cv::resize(
            gray,
            scaledGray,
            cv::Size(qMax(1, w / traditionalDownscale), qMax(1, h / traditionalDownscale)),
            0,
            0,
            cv::INTER_AREA);
        gray = scaledGray;
        if (!panoramaSkyMask.empty()) {
            cv::Mat scaledMask;
            cv::resize(
                panoramaSkyMask,
                scaledMask,
                gray.size(),
                0,
                0,
                cv::INTER_NEAREST);
            panoramaSkyMask = scaledMask;
        }
        w = gray.cols;
        h = gray.rows;
    }
#endif
    if (directYoloEnabled && cfg.detectMedianKernel > 1) {
        const int k = oddKernel(cfg.detectMedianKernel, 3);
        cv::medianBlur(gray, gray, k);
    }

    const int backgroundFrames = qMax(1, cfg.detectBackgroundFrames);
    if (m_detectBgStream != s || m_detectBgSegments != segments ||
        m_detectBgWidth != w || m_detectBgHeight != h || m_detectBgRequired != backgroundFrames) {
        m_detectBgTiles = QVector<DetectBackgroundTile>(segments);
        m_detectBgStream = s;
        m_detectBgSegments = segments;
        m_detectBgWidth = w;
        m_detectBgHeight = h;
        m_detectBgRequired = backgroundFrames;
        m_detectBgReadyTiles = 0;
        m_detectBgAllReadyLogged = false;
        emit logRequested(QStringLiteral("DETECT"),
            QStringLiteral("%1 horizon background init: segments=%2 frames=%3 size=%4x%5")
                .arg(s).arg(segments).arg(backgroundFrames).arg(w).arg(h),
            QStringLiteral("#569CD6"));
    }
    if (tileIndex >= m_detectBgTiles.size()) return;

    DetectBackgroundTile &bgTile = m_detectBgTiles[tileIndex];
    const bool wasReady = bgTile.ready;
    int horizonY = 0;
    updateBackgroundTile(bgTile, gray, backgroundFrames, &horizonY, false);
    if (!wasReady && bgTile.ready) {
        ++m_detectBgReadyTiles;
        if (!m_detectBgAllReadyLogged && m_detectBgReadyTiles >= segments) {
            m_detectBgAllReadyLogged = true;
            emit logRequested(QStringLiteral("DETECT"),
                QStringLiteral("%1 horizon background ready: %2/%3 tiles")
                    .arg(s).arg(m_detectBgReadyTiles).arg(segments),
                QStringLiteral("#6A9955"));
        }
    }
    if (!bgTile.ready || bgTile.background8.empty()) return;

    const int cropSize = cfg.detectCropSize;
    if (panoramaSkyMask.empty() || panoramaSkyMask.size() != gray.size() || cv::countNonZero(panoramaSkyMask) <= 0) return;
    const cv::Mat skyMask = panoramaSkyMask;
    const int skyBottom = clampInt(bgTile.horizonY - cfg.detectSkyMargin, cropSize / 2, h - 1);

    cv::Mat detectionSkyMask = skyMask.clone();
#ifndef DMX_ADVANCED_DETECTION
    const int skyShrinkPixels = qBound(0, cfg.detectSkyShrinkPixels, 256);
    if (skyShrinkPixels > 0) {
        cv::Mat eroded;
        const int k = skyShrinkPixels * 2 + 1;
        const cv::Mat edgeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::erode(skyMask, eroded, edgeKernel);
        if (cv::countNonZero(eroded) > qMax(1024, cv::countNonZero(skyMask) / 5)) {
            detectionSkyMask = eroded;
        }
    }
#endif

    cv::Mat localBg;
    cv::GaussianBlur(gray, localBg, cv::Size(0, 0), 9.0);
    cv::Mat darkResponse;
    cv::subtract(localBg, gray, darkResponse);

    cv::Mat equalized;
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(1.8, cv::Size(8, 8));
    clahe->apply(gray, equalized);
    cv::Mat eqBlur;
    cv::GaussianBlur(equalized, eqBlur, cv::Size(0, 0), 1.2);
    cv::Mat sharp;
    cv::addWeighted(equalized, 1.45, eqBlur, -0.45, 0.0, sharp);
    cv::Mat blackhat;
    {
        const int k = oddKernel(cfg.detectTophatKernel, 9);
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::morphologyEx(sharp, blackhat, cv::MORPH_BLACKHAT, kernel);
    }

    cv::Mat response;
    cv::max(darkResponse, blackhat, response);
    cv::bitwise_and(response, detectionSkyMask, response);

    cv::Scalar mean, stddev;
    cv::meanStdDev(response, mean, stddev, detectionSkyMask);
    double threshold = mean[0] + cfg.detectThresholdK * stddev[0];
    threshold = qMax(threshold, qMax(8.0, (double)cfg.detectMinContrast * 0.40));
    threshold = qMin(threshold, 90.0);

    cv::Mat mask;
    cv::threshold(response, mask, threshold, 255.0, cv::THRESH_BINARY);
    cv::bitwise_and(mask, detectionSkyMask, mask);

    {
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
    }

    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
    QVector<DetectCandidateLocal> candidates;
    candidates.reserve(qMin(count, 4096));
    const bool templateReady = ensureTraditionalTemplate(cfg);
    const int practicalMinArea = qMax(4, cfg.detectMinArea);
    const int practicalMaxArea = qMin(qMax(practicalMinArea, cfg.detectMaxArea), 520);

    for (int i = 1; i < count; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < practicalMinArea || area > practicalMaxArea) continue;
        const int left = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int top = stats.at<int>(i, cv::CC_STAT_TOP);
        const int bw = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int bh = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (bw <= 0 || bh <= 0) continue;
        if (bw > 110 || bh > 110 || bw > cfg.detectCropSize || bh > cfg.detectCropSize) continue;
        const double aspect = (double)bw / qMax(1, bh);
        if (aspect < 0.35 || aspect > 3.1) continue;
        const double compactness = (double)area / qMax(1.0, (double)(bw * bh));
        if (compactness < 0.20) continue;

        const int cx = clampInt((int)qRound(centroids.at<double>(i, 0)), 0, w - 1);
        const int cy = clampInt((int)qRound(centroids.at<double>(i, 1)), 0, h - 1);
        if (detectionSkyMask.at<uchar>(cy, cx) == 0) continue;
        const cv::Rect inner(left, top, bw, bh);

        double responseMax = 0.0;
        cv::minMaxLoc(response(inner), nullptr, &responseMax);
        const double linePenalty = qMax(aspect, 1.0 / qMax(0.001, aspect));
        const double rawCheap = responseMax +
                                compactness * 18.0 +
                                qMin(area, 160) * 0.025 -
                                qMax(0.0, linePenalty - 1.8) * 4.0;
        const double lowerBand = qMax(0.0, (double)cy - (double)h * 0.46) / qMax(1.0, (double)h * 0.25);
        const double cheap = rawCheap - qMin(42.0, lowerBand * 42.0);
        if (rawCheap < 14.0) continue;

        DetectCandidateLocal c;
        c.x = cx;
        c.y = cy;
        c.x1 = left;
        c.y1 = top;
        c.x2 = left + bw;
        c.y2 = top + bh;
        c.area = area;
        c.cheapScore = cheap;
        c.score = cheap;
        c.response = responseMax;
        c.aspect = aspect;
        c.compactness = compactness;
        c.detector = QStringLiteral("traditional_realtime");
        candidates.push_back(c);
    }

    std::sort(candidates.begin(), candidates.end(), [](const DetectCandidateLocal &a, const DetectCandidateLocal &b) {
        return a.cheapScore > b.cheapScore;
    });

    QVector<DetectCandidateLocal> featureShortlist = spatialSelectCandidates(
        candidates, qMax(1, cfg.detectFeatureLimit), cfg.detectFeatureNmsRadius);
    QVector<DetectCandidateLocal> yoloInputCandidates = spatialSelectCandidates(
        candidates, qMax(1, cfg.detectYoloConfirmMaxCandidates), cfg.detectYoloConfirmNmsRadius);

    QVector<DetectCandidateLocal> featureTargets = featureShortlist;
    for (const DetectCandidateLocal &yc : yoloInputCandidates) {
        bool exists = false;
        for (const DetectCandidateLocal &fc : featureTargets) {
            if (fc.x == yc.x && fc.y == yc.y) {
                exists = true;
                break;
            }
        }
        if (!exists) featureTargets.push_back(yc);
    }

    QVector<DetectCandidateLocal> featurePool;
    QVector<DetectCandidateLocal> scoredTargets;
    featurePool.reserve(featureTargets.size());
    scoredTargets.reserve(featureTargets.size());
    for (DetectCandidateLocal c : featureTargets) {
        double contrast = 0.0;
        double centerRing = 0.0;
        pointDiffusionFeatures(gray, response, c.x, c.y, &contrast, &centerRing);
        const double corr = templateReady ? templateCorrelationAt(gray, m_traditionalTemplate, c.x, c.y) : 0.0;
        const double clutter = localClutterScore(gray, response, c.x, c.y);
        const double linePenalty = qMax(c.aspect, 1.0 / qMax(0.001, c.aspect));
        double score = c.response +
                       contrast * 1.35 +
                       centerRing * 1.25 +
                       qMax(0.0, corr) * 105.0 +
                       c.compactness * 13.0 -
                       qMax(0.0, clutter - 8.0) * 3.8;
        if (linePenalty > 2.3) score *= 0.78;
        c.contrast = contrast;
        c.centerRing = centerRing;
        c.templateCorr = corr;
        c.clutter = clutter;
        c.score = score;
        if (clutter > 34.0 && corr < 0.12 && centerRing < 34.0) continue;
        scoredTargets.push_back(c);
        if ((contrast >= 1.0 || centerRing >= 7.0 || corr >= 0.03) && score >= 32.0) {
            featurePool.push_back(c);
        }
    }
    std::sort(featurePool.begin(), featurePool.end(), [](const DetectCandidateLocal &a, const DetectCandidateLocal &b) {
        return a.score > b.score;
    });

    QVector<DetectCandidateLocal> tilePool = featurePool;
    for (const DetectCandidateLocal &yc : scoredTargets) {
        bool yoloSeed = false;
        for (const DetectCandidateLocal &src : yoloInputCandidates) {
            if (src.x == yc.x && src.y == yc.y) {
                yoloSeed = true;
                break;
            }
        }
        if (!yoloSeed) continue;
        bool exists = false;
        for (const DetectCandidateLocal &tc : tilePool) {
            if (tc.x == yc.x && tc.y == yc.y) {
                exists = true;
                break;
            }
        }
        if (!exists) tilePool.push_back(yc);
    }
    std::sort(tilePool.begin(), tilePool.end(), [](const DetectCandidateLocal &a, const DetectCandidateLocal &b) {
        return a.score > b.score;
    });
    const int perTileLimit = qBound(1, qMax(cfg.detectYoloConfirmMaxCandidates * 2, cfg.detectMaxCandidatesPerFrame), 64);
    QVector<DetectCandidateLocal> selected = spatialSelectCandidates(tilePool, perTileLimit, cfg.detectFeatureNmsRadius);

    QVector<DetectCandidateLocal> backgroundProtected = featurePool;
    for (const DetectCandidateLocal &c : selected) backgroundProtected.push_back(c);
    updateBackgroundSlow(bgTile, gray, skyMask, backgroundProtected,
                         cfg.detectBackgroundUpdateAlpha,
                         cfg.detectBackgroundUpdateInterval,
                         cfg.detectBackgroundProtectRadius,
                         cfg.detectSkyMargin);

    if (selected.isEmpty() || !m_cache) return;

    const PanoramaCache::BlockState st = (s == QStringLiteral("BW"))
        ? m_cache->state(PanoramaCache::FullBw)
        : m_cache->state(PanoramaCache::FullRgb);
    const int panoW = st.panoW > 0 ? st.panoW : cfg.fullWidth;
    const int panoH = st.panoH > 0 ? st.panoH : cfg.fullHeight;
    const int actualSliceW = st.sliceW > 0 ? st.sliceW : sliceW;
    const int actualSegments = st.segments > 0 ? st.segments : segments;
    if (panoW <= 0 || panoH <= 0 || actualSegments <= 0 || actualSliceW <= 0 || st.totalFrames == 0) return;
    const quint64 round = ((st.totalFrames - 1) / (quint64)actualSegments) + 1;
    if (round == 0) return;

    DetectRoundStateLocal &roundState = (s == QStringLiteral("BW")) ? m_roundCandidatesBw : m_roundCandidatesRgb;
    if (roundState.round != round ||
        roundState.segments != actualSegments ||
        roundState.sliceW != actualSliceW ||
        roundState.panoW != panoW ||
        roundState.panoH != panoH ||
        roundState.frameW != w ||
        roundState.frameH != h) {
        roundState = DetectRoundStateLocal();
        roundState.round = round;
        roundState.segments = actualSegments;
        roundState.sliceW = actualSliceW;
        roundState.panoW = panoW;
        roundState.panoH = panoH;
        roundState.frameW = w;
        roundState.frameH = h;
    }

    const int baseX = clampInt(tileIndex * actualSliceW, 0, qMax(0, panoW - 1));
    for (const DetectCandidateLocal &c : selected) {
        DetectCandidateLocal out = c;
        out.frameX = c.x;
        out.frameY = c.y;
        out.frameBoxX1 = c.x1;
        out.frameBoxY1 = c.y1;
        out.frameBoxX2 = c.x2;
        out.frameBoxY2 = c.y2;
        out.panoX = clampInt(baseX + (int)((qint64)c.x * actualSliceW / qMax(1, w)), 0, panoW - 1);
        out.panoY = clampInt((int)((qint64)c.y * panoH / qMax(1, h)), 0, panoH - 1);
        out.panoBoxX1 = clampInt(baseX + (int)((qint64)c.x1 * actualSliceW / qMax(1, w)), 0, panoW - 1);
        out.panoBoxY1 = clampInt((int)((qint64)c.y1 * panoH / qMax(1, h)), 0, panoH - 1);
        out.panoBoxX2 = clampInt(baseX + (int)((qint64)c.x2 * actualSliceW / qMax(1, w)), 0, panoW - 1);
        out.panoBoxY2 = clampInt((int)((qint64)c.y2 * panoH / qMax(1, h)), 0, panoH - 1);
        out.tileIndex = tileIndex;
        out.frameW = w;
        out.frameH = h;
        out.sliceW = actualSliceW;
        out.horizonY = bgTile.horizonY;
        out.skyBottom = skyBottom;
        out.round = round;
        out.fileIdx = fileIdx;
        out.rxMs = rxMs;
        out.sourcePath = sourcePath;
        roundState.candidates.push_back(out);
    }
}

void VideoWorker::finalizeRoundCandidates(const QString &stream, quint64 round, quint64 fileIdx, qint64 rxMs, const QString &sourcePath)
{
    if (!m_cache) return;
    const AppConfig &cfg = AppConfig::instance();
    if (!cfg.detectEnabled || cfg.detectSaveRoot.trimmed().isEmpty()) return;

    const QString detectStream = cfg.detectStream.trimmed().toUpper();
    const QString s = (stream == QStringLiteral("GRAY")) ? QStringLiteral("BW") : stream.trimmed().toUpper();
    if (detectStream != QStringLiteral("BOTH") && detectStream != s) return;

    DetectRoundStateLocal &roundState = (s == QStringLiteral("BW")) ? m_roundCandidatesBw : m_roundCandidatesRgb;
    if (round == 0 || roundState.round != round) {
        return;
    }

    QElapsedTimer timer;
    timer.start();

    QVector<DetectCandidateLocal> pool = roundState.candidates;
    std::sort(pool.begin(), pool.end(), [](const DetectCandidateLocal &a, const DetectCandidateLocal &b) {
        return a.score > b.score;
    });

    const int cropSize = qMax(32, cfg.detectCropSize);
    const int panoW = roundState.panoW > 0 ? roundState.panoW : cfg.fullWidth;
    const int panoH = roundState.panoH > 0 ? roundState.panoH : cfg.fullHeight;
    const bool directYoloActive =
#ifdef DMX_ADVANCED_DETECTION
        qEnvironmentVariableIntValue("DMX_DIRECT_YOLO") == 1;
#else
        false;
#endif
    const bool yoloActive = cfg.detectYoloEnabled && !directYoloActive && ensureYoloNet(cfg);
    const QString yoloMode = cfg.detectYoloMode.trimmed().toLower();
    const bool useYoloConfirm = yoloActive && (yoloMode == QStringLiteral("confirm") || yoloMode == QStringLiteral("hybrid"));

    QVector<DetectCandidateLocal> finalKept;
    QVector<DetectCandidateLocal> yoloInputCandidates = spatialSelectRoundCandidates(
        pool, qMax(1, cfg.detectYoloConfirmMaxCandidates), cfg.detectYoloConfirmNmsRadius, panoW);
    QStringList yoloDebugItems;
    double yoloDiagThreshold = cfg.detectYoloConfirmThreshold;

    if (useYoloConfirm) {
        yoloDiagThreshold = qMin(cfg.detectYoloConfirmThreshold, 0.00005);
        int yoloDebugIndex = 0;
        for (const DetectCandidateLocal &classic : yoloInputCandidates) {
            const QString yoloDebugPrefix = QStringLiteral("#%1 px=%2 py=%3 sc=%4 ch=%5 resp=%6 ct=%7 tmpl=%8 clu=%9")
                .arg(++yoloDebugIndex)
                .arg(classic.panoX)
                .arg(classic.panoY)
                .arg(classic.score, 0, 'f', 1)
                .arg(classic.cheapScore, 0, 'f', 1)
                .arg(classic.response, 0, 'f', 2)
                .arg(classic.contrast, 0, 'f', 2)
                .arg(classic.templateCorr, 0, 'f', 3)
                .arg(classic.clutter, 0, 'f', 1);
            const double angle = normalizeAngleLocal((double)classic.panoX / (double)qMax(1, panoW) * 360.0);
            QImage stripe = (s == QStringLiteral("BW"))
                ? m_cache->extractFullBwWindow(angle, cropSize)
                : m_cache->extractFullRgbWindow(angle, cropSize);
            if (stripe.isNull()) {
                yoloDebugItems.push_back(yoloDebugPrefix + QStringLiteral(" reject=no_stripe"));
                continue;
            }
            if (stripe.format() != QImage::Format_RGB32) stripe = stripe.convertToFormat(QImage::Format_RGB32);
            QImage crop = fixedCropRgb32(stripe, cropSize / 2, classic.panoY, cropSize);
            if (crop.isNull()) {
                yoloDebugItems.push_back(yoloDebugPrefix + QStringLiteral(" reject=no_crop"));
                continue;
            }

            YoloRunDiagnosticsLocal yoloRunDiag;
            const QVector<YoloDetectionLocal> detections = runYoloOnBgr(
                m_yoloNet, qimageRgb32ToBgr(crop), 0, 0, cfg, yoloDiagThreshold, &yoloRunDiag);
            const QString yoloRunDiagText = yoloRunDiagnosticsText(yoloRunDiag);
            if (detections.isEmpty()) {
                yoloDebugItems.push_back(yoloDebugPrefix + QStringLiteral(" det=0 ")
                    + yoloRunDiagText + QStringLiteral(" reject=no_det"));
                continue;
            }

            bool hasDrone = false;
            YoloDetectionLocal bestDrone;
            double bestBird = 0.0;
            double bestBirdRaw = 0.0;
            for (const YoloDetectionLocal &d : detections) {
                if (d.classId == 0) {
                    if (!hasDrone || d.weightedConfidence > bestDrone.weightedConfidence) {
                        hasDrone = true;
                        bestDrone = d;
                    }
                } else if (d.classId == 1) {
                    bestBird = qMax(bestBird, d.weightedConfidence);
                    bestBirdRaw = qMax(bestBirdRaw, d.confidence);
                }
            }
            const QString yoloDebugScore = QStringLiteral(" det=%1 %2 drone=%3/%4 bird=%5/%6")
                .arg(detections.size())
                .arg(yoloRunDiagText)
                .arg(hasDrone ? bestDrone.confidence : 0.0, 0, 'f', 5)
                .arg(hasDrone ? bestDrone.weightedConfidence : 0.0, 0, 'f', 5)
                .arg(bestBirdRaw, 0, 'f', 5)
                .arg(bestBird, 0, 'f', 5);
            if (!hasDrone) {
                yoloDebugItems.push_back(yoloDebugPrefix + yoloDebugScore + QStringLiteral(" reject=no_drone"));
                continue;
            }
            if (bestDrone.weightedConfidence < cfg.detectYoloConfirmThreshold) {
                yoloDebugItems.push_back(yoloDebugPrefix + yoloDebugScore + QStringLiteral(" reject=low"));
                continue;
            }
            if (bestDrone.weightedConfidence < bestBird * 1.10) {
                yoloDebugItems.push_back(yoloDebugPrefix + yoloDebugScore + QStringLiteral(" reject=bird"));
                continue;
            }

            DetectCandidateLocal fused = classic;
            fused.detector = QStringLiteral("fused_realtime_round");
            fused.classId = bestDrone.classId;
            fused.className = bestDrone.className;
            fused.yoloScore = bestDrone.weightedConfidence;
            fused.yoloBirdScore = bestBird;
            fused.roiBoxX1 = clampInt(bestDrone.x1, 0, cropSize - 1);
            fused.roiBoxY1 = clampInt(bestDrone.y1, 0, cropSize - 1);
            fused.roiBoxX2 = clampInt(bestDrone.x2, 0, cropSize - 1);
            fused.roiBoxY2 = clampInt(bestDrone.y2, 0, cropSize - 1);
            if (fused.roiBoxX2 <= fused.roiBoxX1 || fused.roiBoxY2 <= fused.roiBoxY1) {
                yoloDebugItems.push_back(yoloDebugPrefix + yoloDebugScore + QStringLiteral(" reject=bad_box"));
                continue;
            }

            const int boxCx = (fused.roiBoxX1 + fused.roiBoxX2) / 2;
            const int boxCy = (fused.roiBoxY1 + fused.roiBoxY2) / 2;
            fused.panoX = wrapPanoX(classic.panoX + boxCx - cropSize / 2, panoW);
            fused.panoY = clampInt(classic.panoY + boxCy - cropSize / 2, 0, panoH - 1);
            fused.panoBoxX1 = wrapPanoX(classic.panoX + fused.roiBoxX1 - cropSize / 2, panoW);
            fused.panoBoxY1 = clampInt(classic.panoY + fused.roiBoxY1 - cropSize / 2, 0, panoH - 1);
            fused.panoBoxX2 = wrapPanoX(classic.panoX + fused.roiBoxX2 - cropSize / 2, panoW);
            fused.panoBoxY2 = clampInt(classic.panoY + fused.roiBoxY2 - cropSize / 2, 0, panoH - 1);
            fused.area = qMax(1, (fused.roiBoxX2 - fused.roiBoxX1) * (fused.roiBoxY2 - fused.roiBoxY1));
            fused.score = qMax(classic.score, classic.cheapScore) + bestDrone.weightedConfidence * 1000.0;
            if (!roundCandidateFarEnough(finalKept, fused, cfg.detectNmsRadius, panoW)) {
                yoloDebugItems.push_back(yoloDebugPrefix + yoloDebugScore + QStringLiteral(" reject=round_nms"));
                continue;
            }
            finalKept.push_back(fused);
            yoloDebugItems.push_back(yoloDebugPrefix + yoloDebugScore
                + QStringLiteral(" keep=1 bx=%1,%2,%3,%4")
                    .arg(fused.roiBoxX1)
                    .arg(fused.roiBoxY1)
                    .arg(fused.roiBoxX2)
                    .arg(fused.roiBoxY2));
            if (finalKept.size() >= cfg.detectMaxCandidatesPerFrame) break;
        }
        if (finalKept.isEmpty() && cfg.detectYoloFallbackTraditionalOnEmpty) {
            finalKept = spatialSelectRoundCandidates(pool, cfg.detectMaxCandidatesPerFrame, cfg.detectNmsRadius, panoW);
            for (DetectCandidateLocal &c : finalKept) {
                c.detector = QStringLiteral("traditional_realtime_round_yolo_fallback");
                setRoiBoxFromPanoBox(&c, cropSize, panoW);
            }
            yoloDebugItems.push_back(QStringLiteral("fallback=traditional count=%1").arg(finalKept.size()));
        }
    } else if (yoloMode != QStringLiteral("sky")) {
        finalKept = spatialSelectRoundCandidates(pool, cfg.detectMaxCandidatesPerFrame, cfg.detectNmsRadius, panoW);
        for (DetectCandidateLocal &c : finalKept) {
            c.detector = QStringLiteral("traditional_realtime_round");
            setRoiBoxFromPanoBox(&c, cropSize, panoW);
        }
    }

    if (!finalKept.isEmpty()) {
        std::sort(finalKept.begin(), finalKept.end(), [](const DetectCandidateLocal &a, const DetectCandidateLocal &b) {
            return a.score > b.score;
        });
    }

    int saved = 0;
#ifdef DMX_ADVANCED_DETECTION
    const bool traditionalDiagnosticOnly =
        directYoloActive &&
        qEnvironmentVariableIntValue("DMX_TRADITIONAL_DIAGNOSTIC_ONLY") == 1;
#else
    const bool traditionalDiagnosticOnly = false;
#endif
    for (const DetectCandidateLocal &c : finalKept) {
        const QString src = c.sourcePath.isEmpty() ? sourcePath : c.sourcePath;
        QDateTime frameDt;
        if (!parseFrameDateTime(src, &frameDt)) {
            frameDt = QDateTime::fromMSecsSinceEpoch(c.rxMs > 0 ? c.rxMs : (rxMs > 0 ? rxMs : QDateTime::currentMSecsSinceEpoch()));
        }
        const QString dateDir = frameDt.date().toString(QStringLiteral("yyyyMMdd"));
        const QString outDir = QDir(cfg.detectSaveRoot).filePath(dateDir + QStringLiteral("/") + hourDirName(frameDt));
        QDir().mkpath(outDir);

        const double angle = normalizeAngleLocal((double)c.panoX / (double)qMax(1, panoW) * 360.0);
        QImage stripe = (s == QStringLiteral("BW"))
            ? m_cache->extractFullBwWindow(angle, cropSize)
            : m_cache->extractFullRgbWindow(angle, cropSize);
        if (stripe.isNull()) continue;
        if (stripe.format() != QImage::Format_RGB32) stripe = stripe.convertToFormat(QImage::Format_RGB32);
        QImage crop = fixedCropRgb32(stripe, cropSize / 2, c.panoY, cropSize);
        if (crop.isNull()) continue;

        const QString base = candidateBaseName(src, c.panoX, c.panoY, c.frameX, c.frameY);
        const QString path = uniqueCandidatePath(outDir, base);
        QImageWriter writer(path, "jpg");
        writer.setQuality(cfg.detectJpegQuality);
        if (!writer.write(crop)) continue;

        QJsonObject rec;
        rec.insert(QStringLiteral("file"), QFileInfo(path).fileName());
        rec.insert(QStringLiteral("path"), path);
        rec.insert(QStringLiteral("source"), QFileInfo(src).fileName());
        rec.insert(QStringLiteral("stream"), s);
        rec.insert(QStringLiteral("date"), dateDir);
        rec.insert(QStringLiteral("hour"), hourDirName(frameDt));
        rec.insert(QStringLiteral("angle"), angle);
        rec.insert(QStringLiteral("panoX"), c.panoX);
        rec.insert(QStringLiteral("panoY"), c.panoY);
        rec.insert(QStringLiteral("panoBoxX1"), c.panoBoxX1);
        rec.insert(QStringLiteral("panoBoxY1"), c.panoBoxY1);
        rec.insert(QStringLiteral("panoBoxX2"), c.panoBoxX2);
        rec.insert(QStringLiteral("panoBoxY2"), c.panoBoxY2);
        rec.insert(QStringLiteral("frameX"), c.frameX);
        rec.insert(QStringLiteral("frameY"), c.frameY);
        rec.insert(QStringLiteral("frameBoxX1"), c.frameBoxX1);
        rec.insert(QStringLiteral("frameBoxY1"), c.frameBoxY1);
        rec.insert(QStringLiteral("frameBoxX2"), c.frameBoxX2);
        rec.insert(QStringLiteral("frameBoxY2"), c.frameBoxY2);
        rec.insert(QStringLiteral("roiBoxX1"), c.roiBoxX1);
        rec.insert(QStringLiteral("roiBoxY1"), c.roiBoxY1);
        rec.insert(QStringLiteral("roiBoxX2"), c.roiBoxX2);
        rec.insert(QStringLiteral("roiBoxY2"), c.roiBoxY2);
        rec.insert(QStringLiteral("roiTargetX"), cropSize / 2);
        rec.insert(QStringLiteral("roiTargetY"), cropSize / 2);
        rec.insert(QStringLiteral("tileIndex"), c.tileIndex);
        rec.insert(QStringLiteral("round"), QString::number(round));
        rec.insert(QStringLiteral("horizonY"), c.horizonY);
        rec.insert(QStringLiteral("skyBottom"), c.skyBottom);
        rec.insert(QStringLiteral("backgroundFrames"), cfg.detectBackgroundFrames);
        rec.insert(QStringLiteral("cropSize"), cropSize);
        rec.insert(QStringLiteral("score"), c.score);
        rec.insert(QStringLiteral("area"), c.area);
        rec.insert(QStringLiteral("clutter"), c.clutter);
        rec.insert(QStringLiteral("detector"), c.detector);
        if (c.classId >= 0) rec.insert(QStringLiteral("classId"), c.classId);
        if (!c.className.isEmpty()) rec.insert(QStringLiteral("className"), c.className);
        if (c.yoloScore > 0.0) rec.insert(QStringLiteral("yoloScore"), c.yoloScore);
        if (c.yoloBirdScore > 0.0) rec.insert(QStringLiteral("yoloBirdScore"), c.yoloBirdScore);
        rec.insert(QStringLiteral("skyMaskMode"), QStringLiteral("temporal_sky_v1"));
        rec.insert(QStringLiteral("fileIdx"), QString::number(c.fileIdx > 0 ? c.fileIdx : fileIdx));
        rec.insert(QStringLiteral("rxMs"), QString::number(c.rxMs > 0 ? c.rxMs : rxMs));
        appendCandidateManifest(outDir, rec);

        ++saved;
        if (!traditionalDiagnosticOnly) {
            emit candidateDetected(s, angle, c.panoX, c.panoY, c.score, path,
                                   c.roiBoxX1, c.roiBoxY1, c.roiBoxX2, c.roiBoxY2,
                                   c.className);
        }
        emit logRequested(
            traditionalDiagnosticOnly ? QStringLiteral("DETECT_AUX") : QStringLiteral("DETECT"),
            QStringLiteral("%1 round=%2 angle=%3 x=%4 y=%5 score=%6 ui=%7 file=%8")
                .arg(s)
                .arg((qulonglong)round)
                .arg(angle, 0, 'f', 1)
                .arg(c.panoX)
                .arg(c.panoY)
                .arg(c.score, 0, 'f', 1)
                .arg(traditionalDiagnosticOnly ? 0 : 1)
                .arg(path),
            traditionalDiagnosticOnly ? QStringLiteral("#808080") : QStringLiteral("#FF5252"));
    }

    emit logRequested(QStringLiteral("DETECT"),
        QStringLiteral("%1 round=%2 pool=%3 yolo_in=%4 saved=%5 elapsed=%6ms")
            .arg(s)
            .arg((qulonglong)round)
            .arg(pool.size())
            .arg(yoloInputCandidates.size())
            .arg(saved)
            .arg(timer.elapsed()),
        saved > 0 ? QStringLiteral("#6A9955") : QStringLiteral("#808080"));

    if (!yoloDebugItems.isEmpty()) {
        emit logRequested(QStringLiteral("YOLODBG"),
            QStringLiteral("%1 round=%2 confirm=%3 diag=%4 %5")
                .arg(s)
                .arg((qulonglong)round)
                .arg(cfg.detectYoloConfirmThreshold, 0, 'f', 4)
                .arg(yoloDiagThreshold, 0, 'f', 4)
                .arg(yoloDebugItems.join(QStringLiteral(" | "))),
            QStringLiteral("#FFD54F"));
    }

    roundState.candidates.clear();
}

bool VideoWorker::handlePathInternal(VideoWorker::PathJob &job, int *retryMs)
{
    if (retryMs) *retryMs = 0;
    if (!m_running) return true;
    QElapsedTimer handleTimer;
    handleTimer.start();
    m_lastSender = job.sender;

    const QString t = job.type.trimmed().toUpper();
    const QString p = job.path.trimmed();
    if (t.isEmpty() || p.isEmpty()) return true;

    m_lastRxType = t;
    m_lastRxPath = p;

    const qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 rxMs = (job.rxMs > 0) ? job.rxMs : wallMs;
    if (job.firstSeenMs <= 0) job.firstSeenMs = wallMs;

    if (m_type == 2) {
        emit pathReceived(t, p, job.sender, rxMs);
        return true;
    }

    if (m_type == 1) {
        if (t != "BW" && t != "GRAY") return true;
    } else if (m_type == 0) {
        if (t != "RGB") return true;
    }

    const QString senderIp = extractSenderIp(job.sender);
    const QString winPath0 = mapDevicePathToWindowsShare(p, senderIp);

    quint64 fileIdx = 0;
    const bool hasIdx = parseTrailingIndexStatic(winPath0, &fileIdx);
    // Count rx packets and seq dup/gap exactly ONCE per frame. handlePathInternal
    // is re-entered on every file-not-ready/decode retry; counting here (gated by
    // seqCounted) keeps the RX/SEQ stats honest instead of inflating them by the
    // number of retries.
    if (!job.seqCounted) {
        job.seqCounted = true;
        ++m_totalRxPackets;
        if (hasIdx) {
            const QString subType = (t == "GRAY") ? "BW" : t;
            updateSeqState(subType, fileIdx, winPath0, rxMs);
        }
    }
    const quint64 cacheFileIdx = hasIdx ? fileIdx : 0;

    QString decErr;
    // BW is a fake-gray RGB JPEG. Do NOT spend a 16M-pixel pass converting it to
    // true Indexed8 gray; treat it exactly like RGB (keep RGB32). This removes the
    // BW decode bottleneck (the toIndexed8Gray pass) so BW keeps up like RGB.
    const bool preferGray = false;
    QString usedPath = winPath0;
    const QFileInfo fi(usedPath);
    const bool exists = fi.exists() && fi.isFile();
    const qint64 sz = exists ? fi.size() : 0;
    const bool stable = (exists && sz > 0 && job.lastSize == sz);
    job.lastSize = sz;
    const qint64 maxWaitMs = 8000;
    if (!stable) {
        if ((wallMs - job.firstSeenMs) < maxWaitMs) {
            if (retryMs) *retryMs = 25;
            return false;
        }
        ++m_totalReadFails;
        const QString detail = QStringLiteral("NOT_READY: %1 exist=%2 size=%3").arg(usedPath).arg(exists ? 1 : 0).arg(sz);
        noteReadFail(t, detail, senderIp, wallMs);
        return true;
    }

    QImage loaded;
    QByteArray rawBytes;
    {
        const bool tee = (m_recordingEnabled && !m_recorder.isNull());
        if (tee) {
            QFile f(usedPath);
            if (!f.open(QIODevice::ReadOnly)) {
                ++job.tries;
                if ((wallMs - job.firstSeenMs) < maxWaitMs && job.tries < 40) {
                    if (retryMs) *retryMs = 30;
                    return false;
                }
                decErr = QStringLiteral("open failed");
            } else {
                rawBytes = f.readAll();
                f.close();
                if (!rawBytes.isEmpty()) {
                    QBuffer buf(&rawBytes);
                    buf.open(QIODevice::ReadOnly);
                    QImageReader reader(&buf);
                    QImage img = reader.read();
                    if (!img.isNull()) {
                        if (preferGray) {
                            img = toIndexed8Gray(img);
                        } else {
                            if (img.format() != QImage::Format_RGB32) img = img.convertToFormat(QImage::Format_RGB32);
                        }
                        loaded = img;
                    } else {
                        decErr = reader.errorString();
                    }
                } else {
                    decErr = QStringLiteral("empty bytes");
                }
            }
        } else {
            QImageReader reader(usedPath);
            QImage img = reader.read();
            if (!img.isNull()) {
                if (!m_loggedFormat) {
                    m_loggedFormat = true;
                    emit logRequested(QStringLiteral("FMT"),
                        QString("type=%1 decoded=Format_%2 size=%3x%4")
                            .arg(m_type).arg((int)img.format()).arg(img.width()).arg(img.height()),
                        QStringLiteral("#FFD54F"));
                }
                if (preferGray) {
                    img = toIndexed8Gray(img);
                } else {
                    if (img.format() != QImage::Format_RGB32) img = img.convertToFormat(QImage::Format_RGB32);
                }
                loaded = img;
            } else {
                decErr = reader.errorString();
            }
        }
    }
    if (loaded.isNull()) {
        ++job.tries;
        if ((wallMs - job.firstSeenMs) < maxWaitMs && job.tries < 40) {
            if (retryMs) *retryMs = 30;
            return false;
        }
        ++m_totalReadFails;
        noteReadFail(t, QStringLiteral("DECODE_FAIL: %1 err=%2").arg(usedPath, decErr), senderIp, wallMs);
        return true;
    }

    if (!rawBytes.isEmpty() && !m_recorder.isNull()) {
        const QString subType = (t == "GRAY") ? QStringLiteral("BW") : t;
        const QString ext = QFileInfo(usedPath).suffix().trimmed().toLower();
        QMetaObject::invokeMethod(
            m_recorder.data(),
            "enqueueFrame",
            Qt::QueuedConnection,
            Q_ARG(QString, subType),
            Q_ARG(quint64, cacheFileIdx),
            Q_ARG(qint64, rxMs),
            Q_ARG(QString, usedPath),
            Q_ARG(QString, job.sender),
            Q_ARG(QString, ext),
            Q_ARG(QByteArray, rawBytes)
        );
    }

    if (m_type == 1) {
        QImage bwFull = loaded;
        if (bwFull.isNull() || bwFull.format() != QImage::Format_RGB32) {
            ++m_totalReadFails;
            noteReadFail(QStringLiteral("BW"), QStringLiteral("toRGB failed: %1").arg(usedPath), senderIp, wallMs);
            return true;
        }
        bwFull = rotateCCW90(bwFull, true);
        if (bwFull.isNull()) {
            ++m_totalReadFails;
            noteReadFail(QStringLiteral("BW"), QStringLiteral("rotate failed: %1").arg(usedPath), senderIp, wallMs);
            return true;
        }

        if (m_cache) {
            m_cache->pushBwFrame(bwFull, cacheFileIdx, usedPath, rxMs, job.angleDeg);
            collectTileCandidates(QStringLiteral("BW"), bwFull, job.angleDeg, cacheFileIdx, rxMs, usedPath);
            maybeFinalizeRoundCandidates(QStringLiteral("BW"), cacheFileIdx, rxMs, usedPath);
            emit cacheUpdated();
        }
        ++m_totalDecodedFrames;
        {
            const quint64 dt = (quint64)handleTimer.elapsed();
            m_handleMsAccum += dt;
            if (dt > m_handleMsMax) m_handleMsMax = dt;
            ++m_handleCount;
        }
        return true;
    }

    QImage rgbFull = loaded.convertToFormat(QImage::Format_RGB32);
    if (rgbFull.isNull()) {
        ++m_totalReadFails;
        noteReadFail(QStringLiteral("RGB"), QStringLiteral("toRGB failed: %1").arg(usedPath), senderIp, wallMs);
        return true;
    }
    rgbFull = rotateCCW90(rgbFull, true);
    if (rgbFull.isNull()) {
        ++m_totalReadFails;
        noteReadFail(QStringLiteral("RGB"), QStringLiteral("rotate failed: %1").arg(usedPath), senderIp, wallMs);
        return true;
    }

    if (m_cache) {
        m_cache->pushRgbFrame(rgbFull, cacheFileIdx, usedPath, rxMs, job.angleDeg);
        collectTileCandidates(QStringLiteral("RGB"), rgbFull, job.angleDeg, cacheFileIdx, rxMs, usedPath);
        maybeFinalizeRoundCandidates(QStringLiteral("RGB"), cacheFileIdx, rxMs, usedPath);
        emit cacheUpdated();
    }
    ++m_totalDecodedFrames;
    {
        const quint64 dt = (quint64)handleTimer.elapsed();
        m_handleMsAccum += dt;
        if (dt > m_handleMsMax) m_handleMsMax = dt;
        ++m_handleCount;
    }
    return true;
}

void VideoThread::onWorkerLogRequested(const QString &type, const QString &msg, const QString &color)
{
    emit logRequested(type, msg, color);
}
