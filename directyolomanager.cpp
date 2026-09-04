#include "directyolomanager.h"

#include "appconfig.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

namespace {

double envDouble(const char *name, double fallback)
{
    bool ok = false;
    const double value = qEnvironmentVariable(name).trimmed().toDouble(&ok);
    return ok ? value : fallback;
}

int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariable(name).trimmed().toInt(&ok);
    return ok ? value : fallback;
}

QString projectDir()
{
    const QString configured = qEnvironmentVariable("DMX_PROJECT_DIR").trimmed();
    if (!configured.isEmpty()) return QDir::cleanPath(configured);
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral(".."));
}

} // namespace

DirectYoloManager::DirectYoloManager(QObject *parent)
    : QObject(parent)
{
    m_enabled = qEnvironmentVariableIntValue("DMX_DIRECT_YOLO") == 1;
    if (m_enabled) startWorker();
}

DirectYoloManager::~DirectYoloManager()
{
    m_stopping = true;
    if (!m_process) return;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->closeWriteChannel();
        m_process->terminate();
        if (!m_process->waitForFinished(1500)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
}

bool DirectYoloManager::isEnabled() const
{
    return m_enabled;
}

void DirectYoloManager::startWorker()
{
    const AppConfig &cfg = AppConfig::instance();
    const QString root = projectDir();
    QString python = qEnvironmentVariable("DMX_DIRECT_YOLO_PYTHON").trimmed();
    if (python.isEmpty()) python = QDir(root).filePath(QStringLiteral(".venv/bin/python"));
    const QString script = QDir(root).filePath(QStringLiteral("tools/dmx_direct_yolo_worker.py"));

    if (!QFileInfo(python).isExecutable() || !QFileInfo(script).isFile()) {
        m_enabled = false;
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            QStringLiteral("worker unavailable: python=%1 script=%2").arg(python, script),
            QStringLiteral("#F44336"));
        return;
    }
    if (!QFileInfo(cfg.detectYoloModelPath).isFile()) {
        m_enabled = false;
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            QStringLiteral("model not found: %1").arg(cfg.detectYoloModelPath),
            QStringLiteral("#F44336"));
        return;
    }

    QStringList args;
    args << script
         << QStringLiteral("--model") << cfg.detectYoloModelPath
         << QStringLiteral("--save-root") << cfg.detectSaveRoot
         << QStringLiteral("--input-size") << QString::number(cfg.detectYoloInputSize)
         << QStringLiteral("--class-names")
         << qEnvironmentVariable("DMX_DIRECT_YOLO_CLASS_NAMES").trimmed()
         << QStringLiteral("--high-threshold")
         << QString::number(envDouble("DMX_DIRECT_YOLO_HIGH_THRESHOLD", 0.12), 'f', 6)
         << QStringLiteral("--low-threshold")
         << QString::number(envDouble("DMX_DIRECT_YOLO_LOW_THRESHOLD", 0.05), 'f', 6)
         << QStringLiteral("--bird-ratio")
         << QString::number(envDouble("DMX_DIRECT_YOLO_BIRD_RATIO", 1.10), 'f', 4)
         << QStringLiteral("--iou-threshold")
         << QString::number(cfg.detectYoloIouThreshold, 'f', 4)
         << QStringLiteral("--sky-coverage")
         << QString::number(envDouble("DMX_DIRECT_YOLO_SKY_COVERAGE", 0.15), 'f', 4)
         << QStringLiteral("--frame-budget-ms")
         << QString::number(envDouble("DMX_DIRECT_YOLO_FRAME_BUDGET_MS", 240.0), 'f', 1)
         << QStringLiteral("--catchup-budget-ms")
         << QString::number(envDouble("DMX_DIRECT_YOLO_CATCHUP_BUDGET_MS", 120.0), 'f', 1)
         << QStringLiteral("--max-queue-delay-ms")
         << QString::number(envDouble("DMX_DIRECT_YOLO_MAX_QUEUE_DELAY_MS", 450.0), 'f', 1)
         << QStringLiteral("--max-output")
         << QString::number(envInt("DMX_DIRECT_YOLO_MAX_OUTPUT", 3));
    if (envInt("DMX_STATIC_CLUTTER", 0) == 1) {
        args << QStringLiteral("--static-clutter")
             << QStringLiteral("--static-min-hits")
             << QString::number(envInt("DMX_STATIC_CLUTTER_MIN_HITS", 5))
             << QStringLiteral("--static-min-duration-ms")
             << QString::number(envInt("DMX_STATIC_CLUTTER_MIN_DURATION_MS", 24000))
             << QStringLiteral("--static-min-context-hits")
             << QString::number(envInt("DMX_STATIC_CLUTTER_MIN_CONTEXT_HITS", 3))
             << QStringLiteral("--static-context-threshold")
             << QString::number(envDouble("DMX_STATIC_CLUTTER_CONTEXT_THRESHOLD", 0.10), 'f', 4);
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->setWorkingDirectory(root);
    connect(m_process, &QProcess::started, this, &DirectYoloManager::onStarted);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &DirectYoloManager::onStdoutReady);
    connect(m_process, &QProcess::readyReadStandardError, this, &DirectYoloManager::onStderrReady);
    connect(m_process,
            QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred),
            this,
            &DirectYoloManager::onProcessError);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &DirectYoloManager::onFinished);
    m_process->start(python, args, QIODevice::ReadWrite);
}

void DirectYoloManager::submitFrame(const QString &stream,
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
                                    int skyShrinkPixels)
{
    if (!m_enabled || !m_process || sourcePath.isEmpty()) return;

    QJsonObject request;
    request.insert(QStringLiteral("type"), QStringLiteral("frame"));
    request.insert(QStringLiteral("requestId"), QString::number(++m_nextRequestId));
    request.insert(QStringLiteral("stream"), stream.trimmed().toUpper());
    request.insert(QStringLiteral("sourcePath"), sourcePath);
    request.insert(QStringLiteral("angleDeg"), angleDeg);
    request.insert(QStringLiteral("fileIdx"), QString::number(fileIdx));
    request.insert(QStringLiteral("rxMs"), QString::number(rxMs));
    request.insert(QStringLiteral("submitMs"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    request.insert(QStringLiteral("tileIndex"), tileIndex);
    request.insert(QStringLiteral("segments"), segments);
    request.insert(QStringLiteral("sliceW"), sliceW);
    request.insert(QStringLiteral("panoW"), panoW);
    request.insert(QStringLiteral("panoH"), panoH);
    request.insert(QStringLiteral("skyMaskPath"), skyMaskPath);
    request.insert(QStringLiteral("skyShrinkPixels"), skyShrinkPixels);
    sendOrQueue(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void DirectYoloManager::sendOrQueue(const QByteArray &line)
{
    if (m_started && m_process->state() == QProcess::Running) {
        m_process->write(line);
        return;
    }
    while (m_pending.size() >= 16) m_pending.dequeue();
    m_pending.enqueue(line);
}

void DirectYoloManager::flushPending()
{
    if (!m_process || m_process->state() != QProcess::Running) return;
    while (!m_pending.isEmpty()) m_process->write(m_pending.dequeue());
}

void DirectYoloManager::onStarted()
{
    m_started = true;
    flushPending();
    emit logRequested(
        QStringLiteral("YOLO_DIRECT"),
        QStringLiteral("GPU worker process started"),
        QStringLiteral("#569CD6"));
}

void DirectYoloManager::onStdoutReady()
{
    if (!m_process) return;
    m_stdoutBuffer += m_process->readAllStandardOutput();
    int newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (!line.isEmpty()) handleOutputLine(line);
    }
}

void DirectYoloManager::handleOutputLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            QStringLiteral("invalid worker output: %1").arg(QString::fromUtf8(line.left(300))),
            QStringLiteral("#F44336"));
        return;
    }

    const QJsonObject root = document.object();
    const QString type = root.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("ready")) {
        QStringList classNames;
        const QJsonArray classes = root.value(QStringLiteral("classes")).toArray();
        for (const QJsonValue &value : classes) classNames.push_back(value.toString());
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            QStringLiteral("ready model=%1 provider=%2 input=%3 decoder=%4 classes=%5 static=%6 warmup=%7ms")
                .arg(root.value(QStringLiteral("model")).toString())
                .arg(root.value(QStringLiteral("provider")).toString())
                .arg(root.value(QStringLiteral("inputShape")).toString())
                .arg(root.value(QStringLiteral("decoder")).toString())
                .arg(classNames.join(QStringLiteral(",")))
                .arg(root.value(QStringLiteral("staticClutter")).toBool() ? 1 : 0)
                .arg(root.value(QStringLiteral("warmupMs")).toDouble(), 0, 'f', 1),
            QStringLiteral("#6A9955"));
        return;
    }
    if (type == QStringLiteral("error")) {
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            root.value(QStringLiteral("message")).toString(),
            QStringLiteral("#F44336"));
        return;
    }
    if (type != QStringLiteral("result")) return;

    const QJsonArray candidates = root.value(QStringLiteral("candidates")).toArray();
    int reportedCount = 0;
    int staticCount = 0;
    for (const QJsonValue &value : candidates) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("staticSuppressed")).toBool()) {
            ++staticCount;
            const bool newlySuppressed = candidate.value(QStringLiteral("staticNewlySuppressed")).toBool();
            emit staticClutterDetected(
                candidate.value(QStringLiteral("stream")).toString(),
                candidate.value(QStringLiteral("className")).toString(),
                candidate.value(QStringLiteral("panoX")).toInt(),
                candidate.value(QStringLiteral("panoY")).toInt(),
                candidate.value(QStringLiteral("staticTrackId")).toInt(),
                candidate.value(QStringLiteral("staticStableHits")).toInt(),
                candidate.value(QStringLiteral("staticContextScore")).toDouble(),
                candidate.value(QStringLiteral("cropPath")).toString());
            emit logRequested(
                QStringLiteral("STATIC_CLUTTER"),
                QStringLiteral("%1 track=%2 class=%3 x=%4 y=%5 hits=%6 context=%7 hash=%8 crop=%9")
                    .arg(newlySuppressed ? QStringLiteral("suppress") : QStringLiteral("hold"))
                    .arg(candidate.value(QStringLiteral("staticTrackId")).toInt())
                    .arg(candidate.value(QStringLiteral("className")).toString())
                    .arg(candidate.value(QStringLiteral("panoX")).toInt())
                    .arg(candidate.value(QStringLiteral("panoY")).toInt())
                    .arg(candidate.value(QStringLiteral("staticStableHits")).toInt())
                    .arg(candidate.value(QStringLiteral("staticContextScore")).toDouble(), 0, 'f', 3)
                    .arg(candidate.value(QStringLiteral("staticHashDistance")).toInt())
                    .arg(candidate.value(QStringLiteral("cropPath")).toString()),
                newlySuppressed ? QStringLiteral("#D19A66") : QStringLiteral("#808080"));
            continue;
        }
        ++reportedCount;
        emit candidateDetected(
            candidate.value(QStringLiteral("stream")).toString(),
            candidate.value(QStringLiteral("angle")).toDouble(),
            candidate.value(QStringLiteral("panoX")).toInt(),
            candidate.value(QStringLiteral("panoY")).toInt(),
            candidate.value(QStringLiteral("score")).toDouble(),
            candidate.value(QStringLiteral("cropPath")).toString(),
            candidate.value(QStringLiteral("roiBoxX1")).toInt(),
            candidate.value(QStringLiteral("roiBoxY1")).toInt(),
            candidate.value(QStringLiteral("roiBoxX2")).toInt(),
            candidate.value(QStringLiteral("roiBoxY2")).toInt(),
            candidate.value(QStringLiteral("className")).toString());
    }

    const QString workerError = root.value(QStringLiteral("error")).toString();
    if (!workerError.isEmpty()) {
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            workerError,
            QStringLiteral("#F44336"));
    }

    if (root.value(QStringLiteral("log")).toBool() || !candidates.isEmpty()) {
        emit logRequested(
            QStringLiteral("YOLO_DIRECT"),
            QStringLiteral("%1 frame=%2 global=%3 local=%4/%5 det=%6 static=%7 "
                           "queue=%8ms decode=%9ms infer=%10ms total=%11ms budget=%12ms")
                .arg(root.value(QStringLiteral("stream")).toString())
                .arg(root.value(QStringLiteral("fileIdx")).toString())
                .arg(root.value(QStringLiteral("globalRuns")).toInt())
                .arg(root.value(QStringLiteral("localRuns")).toInt())
                .arg(root.value(QStringLiteral("localWindows")).toInt())
                .arg(reportedCount)
                .arg(staticCount)
                .arg(root.value(QStringLiteral("queueDelayMs")).toDouble(), 0, 'f', 1)
                .arg(root.value(QStringLiteral("decodeMs")).toDouble(), 0, 'f', 1)
                .arg(root.value(QStringLiteral("inferMs")).toDouble(), 0, 'f', 1)
                .arg(root.value(QStringLiteral("totalMs")).toDouble(), 0, 'f', 1)
                .arg(root.value(QStringLiteral("budgetMs")).toDouble(), 0, 'f', 0),
            reportedCount == 0 ? QStringLiteral("#808080") : QStringLiteral("#FF5252"));
    }
}

void DirectYoloManager::onStderrReady()
{
    if (!m_process) return;
    m_stderrBuffer += m_process->readAllStandardError();
    int newline = -1;
    while ((newline = m_stderrBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stderrBuffer.left(newline).trimmed();
        m_stderrBuffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            emit logRequested(
                QStringLiteral("YOLO_DIRECT"),
                QString::fromUtf8(line.left(500)),
                QStringLiteral("#FFD54F"));
        }
    }
}

void DirectYoloManager::onProcessError(QProcess::ProcessError error)
{
    if (m_stopping) return;
    emit logRequested(
        QStringLiteral("YOLO_DIRECT"),
        QStringLiteral("worker process error=%1: %2").arg((int)error).arg(m_process ? m_process->errorString() : QString()),
        QStringLiteral("#F44336"));
}

void DirectYoloManager::onFinished(int exitCode, QProcess::ExitStatus status)
{
    m_started = false;
    if (m_stopping) return;
    emit logRequested(
        QStringLiteral("YOLO_DIRECT"),
        QStringLiteral("worker stopped exit=%1 status=%2").arg(exitCode).arg((int)status),
        QStringLiteral("#F44336"));
}
