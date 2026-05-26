#include "rawrecorder.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include "asciipath.h"

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

static QString numU64(quint64 v)
{
    return QString::fromLatin1(QByteArray::number(v));
}

static QString numI64(qint64 v)
{
    return QString::fromLatin1(QByteArray::number(v));
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
    if (m_indexFile.isOpen()) m_indexFile.close();
    emit logRequested(QStringLiteral("REC"), QStringLiteral("Start: %1").arg(m_rootDir), QStringLiteral("#569CD6"));
}

void RawRecorder::stopRecording()
{
    QMutexLocker lk(&m_mtx);
    m_enabled = false;
    if (m_indexFile.isOpen()) m_indexFile.close();
    emit logRequested(QStringLiteral("REC"), QStringLiteral("Stop"), QStringLiteral("#569CD6"));
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
    rollIfNeeded(nowMs);

    QString subDir = job.stream.trimmed().toUpper();
    if (subDir != QStringLiteral("RGB") && subDir != QStringLiteral("BW") && subDir != QStringLiteral("GRAY")) subDir = QStringLiteral("UNK");
    if (subDir == QStringLiteral("GRAY")) subDir = QStringLiteral("BW");

    QString ext = job.ext.trimmed().toLower();
    if (ext.isEmpty()) ext = QStringLiteral("bin");
    ext = safeAsciiComponent(ext, QStringLiteral("bin"));

    quint64 seq = 0;
    QString outDir;
    {
        QMutexLocker lk(&m_mtx);
        seq = ++m_seq;
        outDir = m_sessionDir;
    }
    if (outDir.isEmpty()) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Session not ready"), QStringLiteral("#F44336"));
        schedule();
        return;
    }

    const QString dstDir = QDir(outDir).filePath(subDir.toLower());
    if (!QDir().mkpath(dstDir)) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Dir create failed: %1").arg(dstDir), QStringLiteral("#F44336"));
        schedule();
        return;
    }

    const QString name = (job.fileIdx > 0)
        ? (numU64(job.fileIdx) + QStringLiteral("_") + numI64(nowMs) + QStringLiteral(".") + ext)
        : (QStringLiteral("seq_") + numU64(seq) + QStringLiteral("_") + numI64(nowMs) + QStringLiteral(".") + ext);
    const QString dstPath = QDir(dstDir).filePath(name);

    QFile f(dstPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Write failed: %1").arg(dstPath), QStringLiteral("#F44336"));
        schedule();
        return;
    }
    f.write(job.bytes);
    f.close();

    {
        QMutexLocker lk(&m_mtx);
        if (m_indexFile.isOpen()) {
            QJsonObject o;
            o.insert(QStringLiteral("t"), nowMs);
            o.insert(QStringLiteral("stream"), subDir);
            o.insert(QStringLiteral("fileIdx"), (qint64)job.fileIdx);
            o.insert(QStringLiteral("file"), QDir(subDir.toLower()).filePath(name));
            o.insert(QStringLiteral("bytes"), (qint64)job.bytes.size());
            o.insert(QStringLiteral("src"), job.srcPath);
            o.insert(QStringLiteral("sender"), job.sender);
            m_indexFile.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            m_indexFile.write("\n");
            ++m_indexUnflushedCount;
            const qint64 sinceFlush = nowMs - m_lastFlushMs;
            if (m_indexUnflushedCount >= 32 || sinceFlush >= 250) {
                m_indexFile.flush();
                m_indexUnflushedCount = 0;
                m_lastFlushMs = nowMs;
            }
        }
    }

    schedule();
}

void RawRecorder::rollIfNeeded(qint64 nowMs)
{
    bool needNew = false;
    {
        QMutexLocker lk(&m_mtx);
        if (!m_enabled) return;
        if (m_sessionStartMs <= 0 || m_sessionDir.isEmpty()) needNew = true;
        else if (m_rollMinutes > 0 && (nowMs - m_sessionStartMs) >= (qint64)m_rollMinutes * 60 * 1000) needNew = true;
    }
    if (needNew) openNewSession(nowMs);
}

bool RawRecorder::openNewSession(qint64 nowMs)
{
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(nowMs);
    const QString ts = makeAsciiTimestamp(dt, QStringLiteral("REC2_"));
    const QString sessionDir = QDir(m_rootDir).filePath(ts);
    if (!QDir().mkpath(QDir(sessionDir).filePath(QStringLiteral("rgb"))) ||
        !QDir().mkpath(QDir(sessionDir).filePath(QStringLiteral("bw")))) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Dir create failed: %1").arg(sessionDir), QStringLiteral("#F44336"));
        return false;
    }

    QMutexLocker lk(&m_mtx);
    if (m_indexFile.isOpen()) m_indexFile.close();
    m_indexFile.setFileName(QDir(sessionDir).filePath(QStringLiteral("index.jsonl")));
    if (!m_indexFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        emit logRequested(QStringLiteral("REC"), QStringLiteral("Index open failed: %1").arg(sessionDir), QStringLiteral("#F44336"));
        return false;
    }
    m_sessionStartMs = nowMs;
    m_sessionDir = sessionDir;
    m_indexUnflushedCount = 0;
    m_lastFlushMs = nowMs;
    emit logRequested(QStringLiteral("REC"), QStringLiteral("Roll: %1").arg(m_sessionDir), QStringLiteral("#6A9955"));
    return true;
}
