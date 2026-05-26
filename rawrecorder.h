#ifndef RAWRECORDER_H
#define RAWRECORDER_H

#include <QObject>
#include <QAtomicInteger>
#include <QMutex>
#include <QQueue>
#include <QFile>

class RawRecorder : public QObject
{
    Q_OBJECT
public:
    explicit RawRecorder(QObject *parent = nullptr);
    void queueStats(int *count, qint64 *bytes);

public slots:
    void startRecording(const QString &rootDir, int rollMinutes);
    void stopRecording();
    void enqueueFrame(const QString &stream, quint64 fileIdx, qint64 rxMs, const QString &srcPath, const QString &sender, const QString &ext, const QByteArray &bytes);

signals:
    void logRequested(const QString &type, const QString &msg, const QString &color);

private slots:
    void process();

private:
    struct Job {
        QString stream;
        quint64 fileIdx = 0;
        qint64 rxMs = 0;
        QString srcPath;
        QString sender;
        QString ext;
        QByteArray bytes;
    };

    void schedule();
    void rollIfNeeded(qint64 nowMs);
    bool openNewSession(qint64 nowMs);

    QMutex m_mtx;
    QQueue<Job> m_jobs;
    QAtomicInteger<int> m_scheduled = 0;

    bool m_enabled = false;
    QString m_rootDir;
    int m_rollMinutes = 10;
    qint64 m_sessionStartMs = 0;
    QString m_sessionDir;
    QFile m_indexFile;
    quint64 m_seq = 0;
    quint64 m_indexUnflushedCount = 0;
    qint64 m_lastFlushMs = 0;
};

#endif

