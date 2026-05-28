#include "videothread.h"
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
#include <QBuffer>
#include <QThread>
#include <QtGlobal>

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
        QString mount = QString::fromLocal8Bit(qgetenv("DMX_SHARE_MOUNT"));
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
}

void VideoWorker::start()
{
    m_running = true;
    m_emitTimer.start();
    m_lastTextEmitMs = 0;
    m_lastStatMs = 0;
    m_totalRxPackets = 0;
    m_totalDecodedFrames = 0;
    m_totalDroppedPackets = 0;
    m_totalReadFails = 0;
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
    m_lastRxType.clear();
    m_lastRxPath.clear();
    m_pendingType.clear();
    m_pendingPath.clear();
    m_pendingDirty = false;
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
        m_pathSocket = new QUdpSocket(this);
        m_pathSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        if (m_pathSocket->bind(QHostAddress::AnyIPv4, 8001, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            emit logRequested(
                u8s("\xE7\xB3\xBB\xE7\xBB\x9F"),
                QString("Bind 8001 ok local=%1:%2 rcvbuf=%3")
                    .arg(m_pathSocket->localAddress().toString())
                    .arg(m_pathSocket->localPort())
                    .arg(m_pathSocket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong()),
                "#00AAAA"
            );
        } else {
            emit logRequested(u8s("\xE7\xB3\xBB\xE7\xBB\x9F"), QString("Bind 8001 failed err=%1").arg(m_pathSocket->errorString()), "#F44336");
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
    {
        QMutexLocker lk(&m_jobsMtx);
        if (m_hasCurrentJob) {
            job = m_currentJob;
            m_hasCurrentJob = false;
            hasJob = true;
        } else if (!m_jobs.isEmpty()) {
            job = m_jobs.dequeue();
            hasJob = true;
        }
    }
    if (!hasJob) return;

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
    if (!m_running) return;
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
    const int port = m_pathSocket ? (int)m_pathSocket->localPort() : 8001;
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

VideoThread::VideoThread(int type, QSharedPointer<PanoramaCache> cache, QObject *parent)
    : QThread(parent),
      m_type(type),
      m_running(false),
      m_worker(nullptr),
      m_cache(std::move(cache))
{
    QString role;
    if (m_type == 1) role = "8001(BW) path worker";
    else if (m_type == 0) role = "8001(RGB) path worker";
    else role = "8001 path dispatcher";
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
        const QString msg = QString::fromUtf8(datagram).trimmed();
        QString typeStr;
        QString originalPath;
        if (!parsePathPayload(msg, &typeStr, &originalPath)) continue;

        const QString senderStr = QString("%1:%2").arg(sender.toString()).arg(senderPort);
        if (m_type == 2) {
            emit pathReceived(typeStr.trimmed().toUpper(), originalPath.trimmed(), senderStr, rxMs);
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

    const qint64 nowMs = (job.rxMs > 0) ? job.rxMs : QDateTime::currentMSecsSinceEpoch();
    if (job.firstSeenMs <= 0) job.firstSeenMs = nowMs;

    if (m_type == 2) {
        emit pathReceived(t, p, job.sender, nowMs);
        return true;
    }

    if (m_type == 1) {
        if (t != "BW" && t != "GRAY") return true;
    } else if (m_type == 0) {
        if (t != "RGB") return true;
    }

    const QString senderIp = extractSenderIp(job.sender);
    const QString winPath0 = mapDevicePathToWindowsShare(p, senderIp);

    auto parseTrailingIndex = [&](const QString &path, quint64 *out)->bool {
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
    };
    quint64 fileIdx = 0;
    const bool hasIdx = parseTrailingIndex(winPath0, &fileIdx);
    // Count rx packets and seq dup/gap exactly ONCE per frame. handlePathInternal
    // is re-entered on every file-not-ready/decode retry; counting here (gated by
    // seqCounted) keeps the RX/SEQ stats honest instead of inflating them by the
    // number of retries.
    if (!job.seqCounted) {
        job.seqCounted = true;
        ++m_totalRxPackets;
        if (hasIdx) {
            const QString subType = (t == "GRAY") ? "BW" : t;
            updateSeqState(subType, fileIdx, winPath0, nowMs);
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
        if ((nowMs - job.firstSeenMs) < maxWaitMs) {
            if (retryMs) *retryMs = 25;
            return false;
        }
        ++m_totalReadFails;
        const QString detail = QStringLiteral("NOT_READY: %1 exist=%2 size=%3").arg(usedPath).arg(exists ? 1 : 0).arg(sz);
        noteReadFail(t, detail, senderIp, nowMs);
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
                if ((nowMs - job.firstSeenMs) < maxWaitMs && job.tries < 40) {
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
        if ((nowMs - job.firstSeenMs) < maxWaitMs && job.tries < 40) {
            if (retryMs) *retryMs = 30;
            return false;
        }
        ++m_totalReadFails;
        noteReadFail(t, QStringLiteral("DECODE_FAIL: %1 err=%2").arg(usedPath, decErr), senderIp, nowMs);
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
            Q_ARG(qint64, nowMs),
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
            noteReadFail(QStringLiteral("BW"), QStringLiteral("toRGB failed: %1").arg(usedPath), senderIp, nowMs);
            return true;
        }
        bwFull = rotateCCW90(bwFull, true);
        if (bwFull.isNull()) {
            ++m_totalReadFails;
            noteReadFail(QStringLiteral("BW"), QStringLiteral("rotate failed: %1").arg(usedPath), senderIp, nowMs);
            return true;
        }

        if (m_cache) {
            m_cache->pushBwFrame(bwFull, cacheFileIdx, usedPath, job.rxMs, job.angleDeg);
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
        noteReadFail(QStringLiteral("RGB"), QStringLiteral("toRGB failed: %1").arg(usedPath), senderIp, nowMs);
        return true;
    }
    rgbFull = rotateCCW90(rgbFull, true);
    if (rgbFull.isNull()) {
        ++m_totalReadFails;
        noteReadFail(QStringLiteral("RGB"), QStringLiteral("rotate failed: %1").arg(usedPath), senderIp, nowMs);
        return true;
    }

    if (m_cache) {
        m_cache->pushRgbFrame(rgbFull, cacheFileIdx, usedPath, job.rxMs, job.angleDeg);
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
