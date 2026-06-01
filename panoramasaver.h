#ifndef PANORAMASAVER_H
#define PANORAMASAVER_H

#include <QObject>
#include <QQueue>
#include <QMutex>
#include <QAtomicInteger>
#include <QSharedPointer>

class PanoramaCache;

class PanoramaSaver : public QObject
{
    Q_OBJECT
public:
    explicit PanoramaSaver(QSharedPointer<PanoramaCache> cache, QObject *parent = nullptr);

public slots:
    void enqueueSave(QString outDir, int previewJpegQuality, int previewMaxWidth);

signals:
    void saveFinished(quint64 saveId, bool ok, const QString &msg, const QString &outDir);

private slots:
    void process();

private:
    void schedule();

    struct Job {
        quint64 id = 0;
        QString outDir;
        int previewJpegQuality = 95;
        int previewMaxWidth = 8192;
    };

    QSharedPointer<PanoramaCache> m_cache;
    QMutex m_mtx;
    QQueue<Job> m_jobs;
    QAtomicInteger<int> m_scheduled = 0;
    quint64 m_nextId = 1;
};

#endif
