#include "videothread.h"
#include "udpprotocol.h"
#include <QDebug>
#include <QVector>
#include <QStringList>
#include <QFileInfo>
#include <QFile>
#include <QAbstractSocket>
#include <QCoreApplication>
#include <QImageReader>
#include <QBuffer>
#include <QTimer>
#include <QTime>
#include <QMetaObject>
#include <opencv2/opencv.hpp>

static QString mapDevicePathToWindowsShare(QString path)
{
    if (path.startsWith("file://", Qt::CaseInsensitive)) {
        path = path.mid(QString("file://").length());
    }

    if (path.startsWith("/data/")) {
        path = "\\\\192.168.4.1\\data\\" + path.mid(QString("/data/").length());
        path.replace("/", "\\");
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

VideoWorker::VideoWorker(int type)
    : QObject(nullptr), m_type(type)
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
    m_totalReadyReadCalls = 0;
    m_totalDatagramsRead = 0;
    m_lastDatagramLen = 0;
    m_lastSender.clear();
    m_lastStatRxPackets = 0;
    m_lastStatDecodedFrames = 0;
    m_lastStatDroppedPackets = 0;
    m_lastStatReadFails = 0;
    m_lastRxType.clear();
    m_lastRxPath.clear();
    m_pendingType.clear();
    m_pendingPath.clear();
    m_pendingDirty = false;
    m_textFrameIndex = 0;
    m_pathRgbFrameIndex = 0;
    m_pathBwFrameIndex = 0;
    m_rgbJobs.clear();
    m_bwJobs.clear();
    m_nextDecodeRgb = true;

    emit logRequested("系统", QString("监听线程启动 PID=%1").arg(QCoreApplication::applicationPid()), "#00AAAA");

    if (m_type == 2) {
        m_pathSocket = new QUdpSocket(this);
        m_pathSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        if (m_pathSocket->bind(QHostAddress::AnyIPv4, 8001, QUdpSocket::DontShareAddress)) {
            emit logRequested(
                "系统",
                QString("成功绑定 8001 (IPv4) local=%1:%2 rcvbuf=%3")
                    .arg(m_pathSocket->localAddress().toString())
                    .arg(m_pathSocket->localPort())
                    .arg(m_pathSocket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong()),
                "#00AAAA"
            );
        } else {
            emit logRequested(
                "系统",
                QString("错误：8001 绑定失败 err=%1").arg(m_pathSocket->errorString()),
                "#F44336"
            );
        }
        connect(m_pathSocket, SIGNAL(readyRead()), this, SLOT(processPathDatagrams()), Qt::DirectConnection);
    } else {
        m_dataSocket = new QUdpSocket(this);
        int dataPort = (m_type == 1) ? 8002 : 8003;
        m_dataSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        if (m_dataSocket->bind(QHostAddress::AnyIPv4, dataPort, QUdpSocket::DontShareAddress)) {
            emit logRequested(
                "系统",
                QString("成功绑定 %1 (IPv4) local=%2:%3 rcvbuf=%4")
                    .arg(dataPort)
                    .arg(m_dataSocket->localAddress().toString())
                    .arg(m_dataSocket->localPort())
                    .arg(m_dataSocket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong()),
                "#00AAAA"
            );
        } else {
            emit logRequested(
                "系统",
                QString("错误：%1 绑定失败 err=%2").arg(dataPort).arg(m_dataSocket->errorString()),
                "#F44336"
            );
        }
        connect(m_dataSocket, SIGNAL(readyRead()), this, SLOT(processPendingDatagrams()), Qt::DirectConnection);
    }

    m_statTimer = new QTimer(this);
    const int statIntervalMs = (m_type == 2) ? 3000 : 5000;
    m_statTimer->setInterval(statIntervalMs);
    connect(m_statTimer, &QTimer::timeout, this, &VideoWorker::onStatTick);
    m_statTimer->start();

    if (m_type == 3 || m_type == 4) {
        m_decodeTimer = new QTimer(this);
        m_decodeTimer->setInterval(33);
        connect(m_decodeTimer, &QTimer::timeout, this, &VideoWorker::onDecodeTick);
        m_decodeTimer->start();
    }
}

void VideoWorker::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_decodeTimer) {
        m_decodeTimer->stop();
        m_decodeTimer->deleteLater();
        m_decodeTimer = nullptr;
    }
    if (m_statTimer) {
        m_statTimer->stop();
        m_statTimer->deleteLater();
        m_statTimer = nullptr;
    }
    if (m_dataSocket) {
        m_dataSocket->close();
        m_dataSocket->deleteLater();
        m_dataSocket = nullptr;
    }
    if (m_pathSocket) {
        m_pathSocket->close();
        m_pathSocket->deleteLater();
        m_pathSocket = nullptr;
    }
}

void VideoWorker::enqueuePathJob(const QString &type, const QString &path)
{
    if (m_type != 3 && m_type != 4) return;
    if (type.isEmpty() || path.isEmpty()) return;

    PathJob job;
    job.type = type;
    job.path = path;
    job.nextTryMs = m_emitTimer.isValid() ? m_emitTimer.elapsed() : 0;

    const int maxQueue = 1024;
    if (m_type == 3) {
        if (type != "RGB") return;
        if (m_rgbJobs.size() >= maxQueue) {
            m_rgbJobs.dequeue();
            ++m_totalDroppedPackets;
        }
        m_rgbJobs.enqueue(job);
        return;
    }

    if (type != "BW" && type != "GRAY") return;
    if (m_bwJobs.size() >= maxQueue) {
        m_bwJobs.dequeue();
        ++m_totalDroppedPackets;
    }
    m_bwJobs.enqueue(job);
}

void VideoWorker::onStatTick()
{
    const int port = (m_type == 1) ? 8002 : ((m_type == 0) ? 8003 : 8001);
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
        : QString(" last=%1;%2").arg(m_lastRxType, mapDevicePathToWindowsShare(m_lastRxPath));
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
}

void VideoWorker::onDecodeTick()
{
    if (m_type != 3 && m_type != 4) return;

    const qint64 nowMs = m_emitTimer.elapsed();
    const int maxAttempts = 120;

    auto takeReadyJob = [&](QQueue<PathJob> &q, PathJob *out) -> bool {
        if (!out) return false;
        const int n = q.size();
        for (int i = 0; i < n; ++i) {
            PathJob job = q.dequeue();
            if (job.nextTryMs <= nowMs) {
                *out = job;
                return true;
            }
            q.enqueue(job);
        }
        return false;
    };

    auto requeue = [&](QQueue<PathJob> &q, PathJob &job, int delayMs) -> void {
        job.nextTryMs = nowMs + delayMs;
        q.enqueue(job);
    };

    auto processPathJob = [&](PathJob &job) -> bool {
        const QString mappedPath = mapDevicePathToWindowsShare(job.path);
        const QFileInfo fi(mappedPath);
        if (!fi.exists() || fi.size() <= 0) {
            ++job.attempts;
            if (job.attempts >= maxAttempts) {
                ++m_totalReadFails;
                emit logRequested("读取失败", "文件不存在或无权限: " + mappedPath, "#F44336");
                return true;
            }
            job.stableHits = 0;
            requeue((job.type == "RGB") ? m_rgbJobs : m_bwJobs, job, 50);
            return true;
        }

        const qint64 sz = fi.size();
        if (job.lastSize >= 0 && job.lastSize == sz) {
            job.stableHits++;
        } else {
            job.lastSize = sz;
            job.stableHits = 0;
            ++job.attempts;
        }
        if (job.stableHits < 1) {
            requeue((job.type == "RGB") ? m_rgbJobs : m_bwJobs, job, 20);
            return true;
        }

        QFile f(mappedPath);
        if (!f.open(QIODevice::ReadOnly)) {
            ++job.attempts;
            if (job.attempts >= maxAttempts) {
                ++m_totalReadFails;
                emit logRequested("读取失败", "文件不存在或无权限: " + mappedPath, "#F44336");
                return true;
            }
            job.stableHits = 0;
            requeue((job.type == "RGB") ? m_rgbJobs : m_bwJobs, job, 50);
            return true;
        }
        const QByteArray bytes = f.readAll();
        if (bytes.size() != sz) {
            ++job.attempts;
            job.stableHits = 0;
            requeue((job.type == "RGB") ? m_rgbJobs : m_bwJobs, job, 20);
            return true;
        }

        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) {
            ++job.attempts;
            job.stableHits = 0;
            requeue((job.type == "RGB") ? m_rgbJobs : m_bwJobs, job, 50);
            return true;
        }
        QImageReader reader(&buffer);
        reader.setAutoDetectImageFormat(true);
        const QSize srcSize = reader.size();
        const bool needRotateCcw90 = (srcSize.width() == 4096 && srcSize.height() == 4096);
        int scaledW = 256;
        if (srcSize.width() > 0) scaledW = qMax(1, srcSize.width() / 8);
        if (needRotateCcw90) {
            reader.setScaledSize(QSize(scaledW, scaledW));
        } else {
            reader.setScaledSize(QSize(scaledW, 240));
        }
        QImage img = reader.read();
        if (img.isNull()) {
            ++job.attempts;
            job.stableHits = 0;
            if (job.attempts >= maxAttempts) {
                ++m_totalReadFails;
                emit logRequested("读取失败", "图片解码失败: " + mappedPath, "#F44336");
                return true;
            }
            requeue((job.type == "RGB") ? m_rgbJobs : m_bwJobs, job, 50);
            return true;
        }

        if (needRotateCcw90) {
            img = img.transformed(QTransform().rotate(-90), Qt::FastTransformation);
            if (!img.isNull() && (img.width() != scaledW || img.height() != 240)) {
                img = img.scaled(scaledW, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            }
        }

        if (job.type == "RGB") {
            QImage rgb = img.convertToFormat(QImage::Format_RGB32);
            emit frameCaptured(rgb.copy(), mappedPath);
            ++m_totalDecodedFrames;
            return true;
        }

        static QVector<QRgb> s_grayTable;
        if (s_grayTable.isEmpty()) {
            for (int c = 0; c < 256; ++c) s_grayTable.push_back(qRgb(c, c, c));
        }
        QImage gray = img.convertToFormat(QImage::Format_Indexed8);
        gray.setColorTable(s_grayTable);
        emit thermalFrameCaptured(gray.copy(), mappedPath);
        ++m_totalDecodedFrames;
        return true;
    };

    PathJob job;
    if (m_type == 3) {
        if (!takeReadyJob(m_rgbJobs, &job)) return;
        processPathJob(job);
        return;
    }
    if (!takeReadyJob(m_bwJobs, &job)) return;
    processPathJob(job);
}

VideoThread::VideoThread(int type, QObject *parent)
    : QThread(parent), m_type(type), m_running(false), m_worker(nullptr)
{
    QString role;
    if (m_type == 2) role = "8001 路径门址线程";
    else if (m_type == 3) role = "8001 彩色解码线程";
    else if (m_type == 4) role = "8001 黑白解码线程";
    else if (m_type == 1) role = "8002 黑白裸流线程";
    else role = "8003 彩色裸流线程";
    qDebug() << "UDP 监听线程已就绪，类型:" << role;
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

void VideoThread::enqueuePathJob(const QString &type, const QString &path)
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(
        m_worker,
        "enqueuePathJob",
        Qt::QueuedConnection,
        Q_ARG(QString, type),
        Q_ARG(QString, path)
    );
}

void VideoThread::run()
{
    m_running = true;
    m_worker = new VideoWorker(m_type);
    m_worker->moveToThread(this);
    connect(m_worker, &VideoWorker::frameCaptured, this, &VideoThread::frameCaptured, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::thermalFrameCaptured, this, &VideoThread::thermalFrameCaptured, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::pageTablePathReceived, this, &VideoThread::pageTablePathReceived, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::pathJobReceived, this, &VideoThread::pathJobReceived, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::logRequested, this, &VideoThread::onWorkerLogRequested, Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_worker, "start", Qt::DirectConnection);
    exec();
    if (m_worker) {
        m_worker->stop();
        delete m_worker;
        m_worker = nullptr;
    }
}

// 裸流二进制组包引擎 (处理 8002/8003)
void VideoWorker::processPendingDatagrams()
{
    if (!m_dataSocket) return;
    ++m_totalReadyReadCalls;

    const int headerSize = (int)sizeof(ImagePacketHeader);

    auto tryHandleTextPath = [&](const QByteArray &datagram) -> bool {
        const QString msg = QString::fromUtf8(datagram).trimmed();
        if (msg.isEmpty()) return true;

        QString typeStr;
        QString pathStr;
        if (!parsePathPayload(msg, &typeStr, &pathStr)) return true;

        if (m_type == 1) {
            if (typeStr != "BW" && typeStr != "GRAY") return true;
        } else {
            if (typeStr != "RGB") return true;
        }

        m_lastRxType = typeStr;
        m_lastRxPath = pathStr;
        m_pendingType = typeStr;
        m_pendingPath = pathStr;
        if (m_pendingDirty) ++m_totalDroppedPackets;
        m_pendingDirty = true;

        return true;
    };

    auto handleBinary = [&](const QByteArray &datagram) -> void {
        if (datagram.size() < headerSize) return;

        const ImagePacketHeader *header = reinterpret_cast<const ImagePacketHeader*>(datagram.constData());
        if (header->headerCode != 0xFFFF) return;

        const uint32_t imgIdx = header->imageIndex;
        const int payloadSize = datagram.size() - headerSize;
        const char *payloadData = datagram.constData() + headerSize;

        ImageBuffer &buf = m_bufferPool[imgIdx];
        if (buf.totalSize == 0) {
            buf.totalSize = header->totalSize;
            buf.data.reserve(header->totalSize);
        }

        buf.data.append(payloadData, payloadSize);
        buf.receivedBytes += static_cast<uint32_t>(payloadSize);

        if (buf.receivedBytes < buf.totalSize) return;

        std::vector<uchar> bytes(buf.data.begin(), buf.data.end());
        m_bufferPool.remove(imgIdx);

        cv::Mat frame = cv::imdecode(bytes, m_type == 1 ? cv::IMREAD_GRAYSCALE : cv::IMREAD_COLOR);
        if (frame.empty()) return;

        if (m_type == 1) {
            QImage img((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_Indexed8);
            static QVector<QRgb> s_grayTable;
            if (s_grayTable.isEmpty()) {
                for (int c = 0; c < 256; ++c) s_grayTable.push_back(qRgb(c, c, c));
            }
            img.setColorTable(s_grayTable);
            emit frameCaptured(img.copy(), QString());
            return;
        }

        cv::cvtColor(frame, frame, cv::COLOR_BGR2BGRA);
        QImage img((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_RGB32);
        emit frameCaptured(img.copy(), QString());
    };

    while (m_dataSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_dataSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = m_dataSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        if (read <= 0) continue;
        if (read != datagram.size()) datagram.resize((int)read);
        ++m_totalDatagramsRead;
        m_lastDatagramLen = datagram.size();
        m_lastSender = QString("%1:%2").arg(sender.toString()).arg(senderPort);
        ++m_totalRxPackets;

        bool isBinary = false;
        if (datagram.size() >= headerSize) {
            const ImagePacketHeader *header = reinterpret_cast<const ImagePacketHeader*>(datagram.constData());
            isBinary = (header->headerCode == 0xFFFF);
        }

        if (isBinary) {
            handleBinary(datagram);
        } else {
            tryHandleTextPath(datagram);
        }
    }
}

// 8001 路径总机：根据 RGB 和 BW 分发数据
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

        const QString msg = QString::fromUtf8(datagram).trimmed();
        QString typeStr;
        QString originalPath;
        if (!parsePathPayload(msg, &typeStr, &originalPath)) continue;

        const QString mappedPath = mapDevicePathToWindowsShare(originalPath);
        emit pageTablePathReceived(mappedPath);
        m_lastRxType = typeStr;
        m_lastRxPath = originalPath;
        emit pathJobReceived(typeStr, mappedPath);
    }
}

void VideoThread::onWorkerLogRequested(const QString &type, const QString &msg, const QString &color)
{
    emit logRequested(type, msg, color);
}
