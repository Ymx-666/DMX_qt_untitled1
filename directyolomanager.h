#ifndef DIRECTYOLOMANAGER_H
#define DIRECTYOLOMANAGER_H

#include <QObject>
#include <QByteArray>
#include <QProcess>
#include <QQueue>
#include <QString>

class DirectYoloManager : public QObject
{
    Q_OBJECT
public:
    explicit DirectYoloManager(QObject *parent = nullptr);
    ~DirectYoloManager() override;

    bool isEnabled() const;

public slots:
    void submitFrame(const QString &stream,
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

signals:
    void logRequested(const QString &type, const QString &msg, const QString &color);
    void candidateDetected(const QString &stream,
                           double angle,
                           int panoX,
                           int panoY,
                           double score,
                           const QString &cropPath,
                           int roiBoxX1,
                           int roiBoxY1,
                           int roiBoxX2,
                           int roiBoxY2,
                           const QString &className);
    void staticClutterDetected(const QString &stream,
                               const QString &className,
                               int panoX,
                               int panoY,
                               int trackId,
                               int stableHits,
                               double contextScore,
                               const QString &cropPath);

private slots:
    void onStarted();
    void onStdoutReady();
    void onStderrReady();
    void onProcessError(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    void startWorker();
    void sendOrQueue(const QByteArray &line);
    void flushPending();
    void handleOutputLine(const QByteArray &line);

    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
    QQueue<QByteArray> m_pending;
    bool m_enabled = false;
    bool m_started = false;
    bool m_stopping = false;
    quint64 m_nextRequestId = 0;
};

#endif // DIRECTYOLOMANAGER_H
