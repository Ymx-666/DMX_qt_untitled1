#include "rawrecorder.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include "asciipath.h"

namespace {
constexpr int kHalfFrameCount = 8;
constexpr int kJpegQuality = 95;

static QString numU64(quint64 v)
{
    return QString::fromLatin1(QByteArray::number(v));
}

static QString numI64(qint64 v)
{
    return QString::fromLatin1(QByteArray::number(v));
}

static QString normalizedStream(QString stream)
{
    stream = stream.trimmed().toUpper();
    if (stream == QStringLiteral("GRAY")) stream = QStringLiteral("BW");
    if (stream != QStringLiteral("RGB") && stream != QStringLiteral("BW")) stream = QStringLiteral("UNK");
    return stream;
}

static QImage orientFrame(const QImage &src)
{
    if (src.isNull()) return QImage();
    QImage in = src.convertToFormat(QImage::Format_RGB32);
    const int w = in.width();
    const int h = in.height();
    if (w <= 0 || h <= 0) return QImage();

    QImage out(h, w, QImage::Format_RGB32);
    if (out.isNull()) return QImage();
    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        const int dx = h - 1 - y;
        for (int x = 0; x < w; ++x) {
            const int dy = w - 1 - x;
            QRgb *dstLine = reinterpret_cast<QRgb*>(out.scanLine(dy));
            dstLine[dx] = srcLine[x];
        }
    }
    return out;
}

static QImage decodeJpegBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty()) return QImage();
    QBuffer buf;
    buf.setData(bytes);
    if (!buf.open(QIODevice::ReadOnly)) return QImage();
    QImageReader reader(&buf);
    QImage img = reader.read();
    if (img.isNull()) return QImage();
    return img.convertToFormat(QImage::Format_RGB32);
}

static QImage stitchHalfPanorama(const QVector<RawRecorder::BufferedFrame> &frames)
{
    if (frames.isEmpty()) return QImage();

    QVector<QImage> oriented;
    oriented.reserve(frames.size());
    int frameW = 0;
    int frameH = 0;
    for (int i = frames.size() - 1; i >= 0; --i) {
        QImage img = orientFrame(frames.at(i).image);
        if (img.isNull()) return QImage();
        if (oriented.isEmpty()) {
            frameW = img.width();
            frameH = img.height();
        } else if (img.width() != frameW || img.height() != frameH) {
            return QImage();
        }
        oriented.push_back(img);
    }

    QImage out(frameW * oriented.size(), frameH, QImage::Format_RGB32);
    if (out.isNull()) return QImage();
    out.fill(Qt::black);
    QPainter p(&out);
    int x = 0;
    for (const QImage &img : oriented) {
        p.drawImage(x, 0, img);
        x += img.width();
    }
    p.end();
    return out;
}

static bool parseSourceDateTime(const QString &sourceName, qint64 fallbackMs, QDateTime *dt)
{
    const QString stem = QFileInfo(sourceName).completeBaseName();
    const QStringList parts = stem.split(QLatin1Char('_'));
    if (parts.size() >= 3) {
        const QDate date = QDate::fromString(parts.at(1), QStringLiteral("yyyyMMdd"));
        const QTime time = QTime::fromString(parts.at(2).left(6), QStringLiteral("HHmmss"));
        if (date.isValid() && time.isValid()) {
            if (dt) *dt = QDateTime(date, time);
            return true;
        }
    }
    if (dt) *dt = QDateTime::fromMSecsSinceEpoch(fallbackMs > 0 ? fallbackMs : QDateTime::currentMSecsSinceEpoch());
    return false;
}

static QString sourceBaseStem(const RawRecorder::BufferedFrame &frame, const QString &stream)
{
    QString stem = QFileInfo(frame.sourceName).completeBaseName().trimmed();
    if (!stem.isEmpty()) return stem;

    QDateTime dt;
    parseSourceDateTime(frame.sourceName, frame.rxMs, &dt);
    const QString idx = frame.fileIdx > 0 ? numU64(frame.fileIdx) : numI64(frame.rxMs);
    return QStringLiteral("%1_%2_%3_%4")
        .arg(stream)
        .arg(dt.date().toString(QStringLiteral("yyyyMMdd")))
        .arg(dt.time().toString(QStringLiteral("HHmmss")))
        .arg(idx);
}

static QString outputDirForFrame(const QString &rootDir, const QString &stream, const RawRecorder::BufferedFrame &frame)
{
    QDateTime dt;
    parseSourceDateTime(frame.sourceName, frame.rxMs, &dt);
    const QString day = dt.date().toString(QStringLiteral("yyyyMMdd"));
    const QString hour = dt.time().toString(QStringLiteral("HH"));
    return QDir(rootDir).filePath(day + QStringLiteral("/") + stream.toLower() + QStringLiteral("/") + hour);
}
} // namespace

RawRecorder::RawRecorder(QObject *parent) : QObject(parent)
{
}

void RawRecorder::queueStats(int *count, qint64 *bytes)
{
    QMutexLocker lk(&m_mtx);
    int c = m_jobs.size();
    qint64 b = 0;
    for (const Job &j : m_jobs) b += j.bytes.size();
    if (count) *count = c;
    if (bytes) *bytes = b;
}

void RawRecorder::startRecording(const QString &rootDir, int rollMinutes)
{
    QMutexLocker lk(&m_mtx);
    m_rootDir = rootDir;
    m_rollMinutes = (rollMinutes > 0) ? rollMinutes : 10;
    m_enabled = true;
    m_sessionStartMs = 0;
    m_sessionDir.clear();
    m_seq = 0;
    m_indexUnflushedCount = 0;
    m_lastFlushMs = 0;
    m_rgbState = StreamState();
    m_bwState = StreamState();
    m_jobs.clear();
    if (m_indexFile.isOpen()) m_indexFile.close();
    emit logRequested(QStringLiteral("REC"), QStringLiteral("Start AB JPG: %1").arg(m_rootDir), QStringLiteral("#569CD6"));
}

void RawRecorder::stopRecording()
{
    QMutexLocker lk(&m_mtx);
    m_enabled = false;
    const int leftCount = m_jobs.size();
    qint64 leftBytes = 0;
    for (const Job &j : m_jobs) leftBytes += j.bytes.size();
    const int leftFrames = m_rgbState.frames.size() + m_bwState.frames.size();
    m_jobs.clear();
    m_rgbState = StreamState();
    m_bwState = StreamState();
    if (m_indexFile.isOpen()) m_indexFile.close();
    emit logRequested(QStringLiteral("REC"),
        QStringLiteral("Stop (dropped queue: %1 jobs, %2 KB, buffered frames: %3)").arg(leftCount).arg(leftBytes / 1024).arg(leftFrames),
        QStringLiteral("#569CD6"));
}

void RawRecorder::enqueueFrame(const QString &stream, quint64 fileIdx, qint64 rxMs, const QString &srcPath, const QString &sender, const QString &ext, const QByteArray &bytes)
{
    if (bytes.isEmpty()) return;
    {
        QMutexLocker lk(&m_mtx);
        if (!m_enabled) return;
        Job j;
        j.stream = stream;
        j.fileIdx = fileIdx;
        j.rxMs = rxMs;
        j.srcPath = srcPath;
        j.sender = sender;
        j.ext = ext;
        j.bytes = bytes;
        m_jobs.enqueue(std::move(j));
    }
    schedule();
}

void RawRecorder::schedule()
{
    if (m_scheduled.testAndSetAcquire(0, 1)) {
        QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
    }
}

void RawRecorder::process()
{
    m_scheduled.storeRelease(0);

    Job job;
    {
        QMutexLocker lk(&m_mtx);
        if (!m_enabled) {
            m_jobs.clear();
            return;
        }
        if (m_jobs.isEmpty()) return;
        job = m_jobs.dequeue();
    }

    const qint64 nowMs = (job.rxMs > 0) ? job.rxMs : QDateTime::currentMSecsSinceEpoch();
    const QString stream = normalizedStream(job.stream);
    if (stream == QStringLiteral("UNK")) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Unknown stream: %1").arg(job.stream), QStringLiteral("#F44336"));
        schedule();
        return;
    }

    QImage decoded = decodeJpegBytes(job.bytes);
    if (decoded.isNull()) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Decode failed: %1").arg(job.srcPath), QStringLiteral("#F44336"));
        schedule();
        return;
    }

    BufferedFrame frame;
    frame.image = decoded;
    frame.sourceName = QFileInfo(job.srcPath).fileName();
    frame.sourcePath = job.srcPath;
    frame.fileIdx = job.fileIdx;
    frame.rxMs = nowMs;
    processDecodedFrame(stream, frame, nowMs);

    schedule();
}

bool RawRecorder::processDecodedFrame(const QString &stream, const BufferedFrame &frame, qint64 nowMs)
{
    StreamState *state = nullptr;
    if (stream == QStringLiteral("RGB")) state = &m_rgbState;
    else if (stream == QStringLiteral("BW")) state = &m_bwState;
    if (!state) return false;

    state->frames.push_back(frame);
    while (state->frames.size() >= kHalfFrameCount) {
        QVector<BufferedFrame> halfFrames;
        halfFrames.reserve(kHalfFrameCount);
        for (int i = 0; i < kHalfFrameCount; ++i) halfFrames.push_back(state->frames.at(i));
        state->frames.erase(state->frames.begin(), state->frames.begin() + kHalfFrameCount);
        if (!writeHalfPanorama(stream, *state, halfFrames, nowMs)) return false;
    }
    return true;
}

bool RawRecorder::writeHalfPanorama(const QString &stream, StreamState &state, const QVector<BufferedFrame> &halfFrames, qint64 nowMs)
{
    if (halfFrames.size() != kHalfFrameCount) return false;
    if (state.nextHalfA || state.groupBaseStem.isEmpty()) {
        state.groupBaseStem = sourceBaseStem(halfFrames.first(), stream);
        ++state.groupIndex;
    }

    const QString suffix = state.nextHalfA ? QStringLiteral("A") : QStringLiteral("B");
    const QString dstDir = outputDirForFrame(m_rootDir, stream, halfFrames.first());
    if (!QDir().mkpath(dstDir)) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Dir create failed: %1").arg(dstDir), QStringLiteral("#F44336"));
        return false;
    }

    QImage pano = stitchHalfPanorama(halfFrames);
    if (pano.isNull()) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Stitch failed: %1 %2").arg(stream, state.groupBaseStem), QStringLiteral("#F44336"));
        return false;
    }

    const QString fileName = state.groupBaseStem + QStringLiteral("-") + suffix + QStringLiteral(".jpg");
    const QString dstPath = QDir(dstDir).filePath(fileName);
    QImageWriter writer(dstPath, "jpg");
    writer.setQuality(kJpegQuality);
    if (!writer.write(pano)) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Write failed: %1 (%2)").arg(dstPath, writer.errorString()), QStringLiteral("#F44336"));
        return false;
    }

    emit logRequested(QStringLiteral("REC"),
        QStringLiteral("AB %1 group=%2 half=%3 frames=%4 file=%5")
            .arg(stream)
            .arg((qulonglong)state.groupIndex)
            .arg(suffix)
            .arg(halfFrames.size())
            .arg(dstPath),
        QStringLiteral("#6A9955"));

    if (!state.nextHalfA) state.groupBaseStem.clear();
    state.nextHalfA = !state.nextHalfA;
    Q_UNUSED(nowMs);
    return true;
}
