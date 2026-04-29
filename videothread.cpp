#include "videothread.h"
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
#include <QFileInfo>
#include <QImageReader>
#include <QThread>
#include <QtGlobal>
#include <opencv2/opencv.hpp>

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
        const QString ip = senderIp.isEmpty() ? QString("192.168.4.1") : senderIp;
        path = "\\\\" + ip + "\\data\\" + path.mid(QString("/data/").length());
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
                *outImg = img;
                return true;
            }
            lastErr = reader.errorString();
        }

        {
            const int flag = preferGray ? cv::IMREAD_GRAYSCALE : cv::IMREAD_COLOR;
            cv::Mat mat = cv::imread(path.toStdString(), flag);
            if (!mat.empty()) {
                if (preferGray) {
                    if (mat.type() != CV_8UC1) {
                        cv::cvtColor(mat, mat, cv::COLOR_BGR2GRAY);
                    }
                    QImage img((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Indexed8);
                    img.setColorTable(grayColorTable());
                    *outImg = img.copy();
                    return !outImg->isNull();
                } else {
                    if (mat.type() != CV_8UC3) {
                        cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR);
                    }
                    cv::cvtColor(mat, mat, cv::COLOR_BGR2BGRA);
                    QImage img((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_RGB32);
                    *outImg = img.copy();
                    return !outImg->isNull();
                }
            }
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
    m_rawMaxIndexSeen = 0;
    m_rawRxCounter = 0;
    m_panorama = QImage();
    m_fullSliceW = 0;
    m_fullSliceH = 0;
    for (QReadWriteLock *l : m_segLocks) delete l;
    m_segLocks.clear();
    if (m_lockBuckets <= 0) m_lockBuckets = 64;
    m_segLocks.reserve(m_lockBuckets);
    for (int i = 0; i < m_lockBuckets; ++i) m_segLocks.push_back(new QReadWriteLock());

    emit logRequested("系统", QString("监听线程启动 PID=%1").arg(QCoreApplication::applicationPid()), "#00AAAA");

    m_statTimer = new QTimer(this);
    const int statIntervalMs = 3000;
    m_statTimer->setInterval(statIntervalMs);
    connect(m_statTimer, &QTimer::timeout, this, &VideoWorker::onStatTick);
    m_statTimer->start();

    if (m_type == 2) {
        m_pathSocket = new QUdpSocket(this);
        m_pathSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        if (m_pathSocket->bind(QHostAddress::AnyIPv4, 8001, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            emit logRequested(
                "系统",
                QString("成功绑定 8001 (IPv4) local=%1:%2 rcvbuf=%3")
                    .arg(m_pathSocket->localAddress().toString())
                    .arg(m_pathSocket->localPort())
                    .arg(m_pathSocket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong()),
                "#00AAAA"
            );
        } else {
            emit logRequested("系统", QString("错误：无法绑定 8001 err=%1").arg(m_pathSocket->errorString()), "#F44336");
        }
        connect(m_pathSocket, SIGNAL(readyRead()), this, SLOT(processPathDatagrams()), Qt::DirectConnection);
    }

    if (m_type == 0 || m_type == 1) {
        const int panoW = 65536;
        const int panoH = 4096;
        if (m_type == 0) {
            m_panorama = QImage(panoW, panoH, QImage::Format_RGB32);
            if (!m_panorama.isNull()) {
                m_panorama.fill(Qt::black);
            } else {
                emit logRequested("错误", QString("RGB 全景缓冲区申请失败: %1x%2").arg(panoW).arg(panoH), "#F44336");
            }
        } else {
            m_panorama = QImage(panoW, panoH, QImage::Format_Indexed8);
            if (!m_panorama.isNull()) {
                m_panorama.setColorTable(grayColorTable());
                m_panorama.fill(0);
            } else {
                emit logRequested("错误", QString("BW 全景缓冲区申请失败: %1x%2").arg(panoW).arg(panoH), "#F44336");
            }
        }
    }
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
    for (QReadWriteLock *l : m_segLocks) delete l;
    m_segLocks.clear();
}

void VideoWorker::requestRoi(double angleDeg, int tag)
{
    if (m_type != 0 && m_type != 1) return;
    if (m_panorama.isNull() || m_fullSliceW <= 0 || m_fullSliceH <= 0) return;

    const int sliceW = m_fullSliceW;
    const int panoW = m_panorama.width();
    const int panoH = m_panorama.height();
    const int segments = qMax(1, panoW / sliceW);
    const double a = normalize360(angleDeg);
    int tileIndex = (int)(a / 360.0 * segments);
    if (tileIndex < 0) tileIndex = 0;
    if (tileIndex >= segments) tileIndex = segments - 1;
    const int startX = tileIndex * sliceW;
    QReadWriteLock *lockA = nullptr;
    QReadWriteLock *lockB = nullptr;
    if (!m_segLocks.isEmpty()) {
        lockA = m_segLocks[bucketIndex(tileIndex, m_segLocks.size())];
    }

    QImage roi(sliceW, panoH, m_panorama.format());
    if (roi.isNull()) return;
    if (roi.format() == QImage::Format_Indexed8) {
        roi.setColorTable(grayColorTable());
    }

    const int rightW = qMin(sliceW, panoW - startX);
    const int leftW = sliceW - rightW;
    const int bpp = (m_panorama.format() == QImage::Format_Indexed8) ? 1 : 4;
    if (leftW > 0 && !m_segLocks.isEmpty()) {
        lockB = m_segLocks[bucketIndex(0, m_segLocks.size())];
    }
    lockTwoRead(lockA, lockB);
    for (int y = 0; y < panoH; ++y) {
        const uchar *srcLine = m_panorama.constScanLine(y);
        uchar *dstLine = roi.scanLine(y);
        memcpy(dstLine, srcLine + startX * bpp, rightW * bpp);
        if (leftW > 0) {
            memcpy(dstLine + rightW * bpp, srcLine, leftW * bpp);
        }
    }
    unlockTwoRead(lockA, lockB);
    emit roiCaptured(roi.copy(), tag);
}

void VideoWorker::requestPanoramaSnapshot()
{
    if (m_type != 0 && m_type != 1) return;
    if (m_panorama.isNull()) return;
    if (m_segLocks.isEmpty()) {
        emit panoramaSnapshotReady(m_panorama.copy());
        return;
    }
    for (QReadWriteLock *l : m_segLocks) if (l) l->lockForRead();
    QImage snap = m_panorama.copy();
    for (int i = m_segLocks.size() - 1; i >= 0; --i) if (m_segLocks[i]) m_segLocks[i]->unlock();
    emit panoramaSnapshotReady(snap);
}

void VideoWorker::onStatTick()
{
    const int port = m_dataSocket ? (int)m_dataSocket->localPort() : 8001;
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
}

VideoThread::VideoThread(int type, QObject *parent)
    : QThread(parent), m_type(type), m_running(false), m_worker(nullptr)
{
    QString role;
    if (m_type == 1) role = "8001(BW) 文件共享线程";
    else if (m_type == 0) role = "8001(RGB) 文件共享线程";
    else role = "8001 路径信令线程";
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

void VideoThread::requestRoi(double angleDeg, int tag)
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(
        m_worker,
        "requestRoi",
        Qt::QueuedConnection,
        Q_ARG(double, angleDeg),
        Q_ARG(int, tag)
    );
}

void VideoThread::requestPanoramaSnapshot()
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(
        m_worker,
        "requestPanoramaSnapshot",
        Qt::QueuedConnection
    );
}

void VideoThread::enqueuePath(QString typeStr, QString pathStr, QString sender)
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(
        m_worker,
        "enqueuePath",
        Qt::QueuedConnection,
        Q_ARG(QString, typeStr),
        Q_ARG(QString, pathStr),
        Q_ARG(QString, sender)
    );
}

void VideoThread::run()
{
    m_running = true;
    m_worker = new VideoWorker(m_type);
    m_worker->moveToThread(this);
    connect(m_worker, &VideoWorker::frameCaptured, this, &VideoThread::frameCaptured, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::thermalFrameCaptured, this, &VideoThread::thermalFrameCaptured, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::pathReceived, this, &VideoThread::pathReceived, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::roiCaptured, this, &VideoThread::roiCaptured, Qt::QueuedConnection);
    connect(m_worker, &VideoWorker::panoramaSnapshotReady, this, &VideoThread::panoramaSnapshotReady, Qt::QueuedConnection);
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

    auto tryHandleTextPath = [&](const QByteArray &datagram, const QString &sender) -> bool {
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

        enqueuePath(typeStr, pathStr, sender);

        return true;
    };

    auto handleBinary = [&](const QByteArray &datagram) -> void {
        if (datagram.size() < headerSize) return;

        const ImagePacketHeader *header = reinterpret_cast<const ImagePacketHeader*>(datagram.constData());
        if (header->headerCode != 0xFFFF) return;

        const uint32_t imgIdx = header->imageIndex;
        const int payloadSize = datagram.size() - headerSize;
        const char *payloadData = datagram.constData() + headerSize;

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        ++m_rawRxCounter;
        if (m_rawMaxIndexSeen != 0 && imgIdx + 1000U < m_rawMaxIndexSeen) {
            m_bufferPool.clear();
            m_rawMaxIndexSeen = imgIdx;
        } else if (imgIdx > m_rawMaxIndexSeen) {
            m_rawMaxIndexSeen = imgIdx;
        }

        ImageBuffer &buf = m_bufferPool[imgIdx];
        if (buf.totalSize == 0) {
            buf.totalSize = header->totalSize;
            buf.data.reserve(header->totalSize);
            buf.createdMs = nowMs;
            buf.lastUpdateMs = nowMs;
            buf.lastProgressBytes = 0;
        }

        buf.data.append(payloadData, payloadSize);
        buf.receivedBytes += static_cast<uint32_t>(payloadSize);
        buf.lastUpdateMs = nowMs;

        if (m_rawRxCounter % 64 == 0 || m_bufferPool.size() > 128) {
            const uint32_t window = 30;
            const uint32_t cutoff = (m_rawMaxIndexSeen > window) ? (m_rawMaxIndexSeen - window) : 0;
            auto it = m_bufferPool.begin();
            while (it != m_bufferPool.end() && it.key() < cutoff) {
                it = m_bufferPool.erase(it);
            }
            const qint64 stallMs = 1500;
            const qint64 hardMs = 5000;
            it = m_bufferPool.begin();
            while (it != m_bufferPool.end()) {
                ImageBuffer &b = it.value();
                const bool noProgress = (b.receivedBytes == b.lastProgressBytes);
                if (noProgress) {
                    if ((b.lastUpdateMs > 0 && nowMs - b.lastUpdateMs > stallMs) ||
                        (b.createdMs > 0 && nowMs - b.createdMs > hardMs)) {
                        it = m_bufferPool.erase(it);
                        continue;
                    }
                } else {
                    b.lastProgressBytes = b.receivedBytes;
                }
                ++it;
            }
        }

        if (buf.receivedBytes < buf.totalSize) return;

        std::vector<uchar> bytes(buf.data.begin(), buf.data.end());
        m_bufferPool.remove(imgIdx);

        cv::Mat frame = cv::imdecode(bytes, m_type == 1 ? cv::IMREAD_GRAYSCALE : cv::IMREAD_COLOR);
        if (frame.empty()) return;

        if (m_type == 1) {
            QImage bw((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_Indexed8);
            bw.setColorTable(grayColorTable());
            QImage bwFull = bw.copy();

            if (m_fullSliceW <= 0 || m_fullSliceH <= 0) {
                m_fullSliceW = bwFull.width();
                m_fullSliceH = bwFull.height();
            }
            const int segments = (m_fullSliceW > 0) ? qMax(1, m_panorama.width() / m_fullSliceW) : 1;
            if (!m_panorama.isNull() && m_fullSliceW > 0) {
                const int panoW = m_panorama.width();
                const int panoH = m_panorama.height();
                const int sliceW = m_fullSliceW;

                const int tileIndex = (int)(imgIdx % (uint32_t)segments);
                const int shiftedIndex = (tileIndex + (segments / 2)) % segments;
                const int startX = shiftedIndex * sliceW;

                QImage src = bwFull;
                if (src.width() != sliceW || src.height() != panoH) {
                    src = src.scaled(sliceW, panoH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                    src = src.convertToFormat(QImage::Format_Indexed8);
                    src.setColorTable(grayColorTable());
                }

                const int rightW = qMin(sliceW, panoW - startX);
                const int leftW = sliceW - rightW;
                QReadWriteLock *lockA = nullptr;
                QReadWriteLock *lockB = nullptr;
                if (!m_segLocks.isEmpty()) {
                    lockA = m_segLocks[bucketIndex(shiftedIndex, m_segLocks.size())];
                    if (leftW > 0) lockB = m_segLocks[bucketIndex(0, m_segLocks.size())];
                }
                lockTwoWrite(lockA, lockB);
                for (int y = 0; y < panoH; ++y) {
                    const uchar *srcLine = src.constScanLine(y);
                    uchar *dstLine = m_panorama.scanLine(y);
                    memcpy(dstLine + startX, srcLine, rightW);
                    if (leftW > 0) {
                        memcpy(dstLine, srcLine + rightW, leftW);
                    }
                }
                unlockTwoWrite(lockA, lockB);
            }

            const int uiPanoW = 8192;
            int previewW = qMax(1, uiPanoW / segments);
            QImage preview = bwFull.scaled(previewW, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            preview = preview.convertToFormat(QImage::Format_Indexed8);
            preview.setColorTable(grayColorTable());
            const double angleDeg = (double)(imgIdx % (uint32_t)segments) * 360.0 / (double)segments;
            emit thermalFrameCaptured(preview.copy(), angleDeg);
            ++m_totalDecodedFrames;
            return;
        }

        cv::cvtColor(frame, frame, cv::COLOR_BGR2BGRA);
        QImage img((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_RGB32);
        QImage rgbFull = img.copy();
        if (m_fullSliceW <= 0 || m_fullSliceH <= 0) {
            m_fullSliceW = rgbFull.width();
            m_fullSliceH = rgbFull.height();
        }
        const int segments = (m_fullSliceW > 0) ? qMax(1, m_panorama.width() / m_fullSliceW) : 1;
        if (!m_panorama.isNull() && m_fullSliceW > 0) {
            const int panoW = m_panorama.width();
            const int panoH = m_panorama.height();
            const int sliceW = m_fullSliceW;
            const int tileIndex = (int)(imgIdx % (uint32_t)segments);
            const int startX = tileIndex * sliceW;

            QImage src = rgbFull;
            if (src.width() != sliceW || src.height() != panoH) {
                src = src.scaled(sliceW, panoH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                src = src.convertToFormat(QImage::Format_RGB32);
            }

            const int rightW = qMin(sliceW, panoW - startX);
            const int leftW = sliceW - rightW;
            QReadWriteLock *lockA = nullptr;
            QReadWriteLock *lockB = nullptr;
            if (!m_segLocks.isEmpty()) {
                lockA = m_segLocks[bucketIndex(tileIndex, m_segLocks.size())];
                if (leftW > 0) lockB = m_segLocks[bucketIndex(0, m_segLocks.size())];
            }
            lockTwoWrite(lockA, lockB);
            for (int y = 0; y < panoH; ++y) {
                const uchar *srcLine = src.constScanLine(y);
                uchar *dstLine = m_panorama.scanLine(y);
                memcpy(dstLine + startX * 4, srcLine, rightW * 4);
                if (leftW > 0) {
                    memcpy(dstLine, srcLine + rightW * 4, leftW * 4);
                }
            }
            unlockTwoWrite(lockA, lockB);
        }

        const int uiPanoW = 8192;
        int previewW = qMax(1, uiPanoW / segments);
        QImage preview = rgbFull.scaled(previewW, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        const double angleDeg = (double)(imgIdx % (uint32_t)segments) * 360.0 / (double)segments;
        emit frameCaptured(preview.copy(), angleDeg);
        ++m_totalDecodedFrames;
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
            tryHandleTextPath(datagram, QString("%1:%2").arg(sender.toString()).arg(senderPort));
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

        enqueuePath(typeStr, originalPath, QString("%1:%2").arg(sender.toString()).arg(senderPort));
    }
}

void VideoWorker::enqueuePath(QString typeStr, QString pathStr, QString sender)
{
    if (!m_running) return;
    ++m_totalRxPackets;
    m_lastSender = sender;

    const QString t = typeStr.trimmed().toUpper();
    const QString p = pathStr.trimmed();
    if (t.isEmpty() || p.isEmpty()) return;

    m_lastRxType = t;
    m_lastRxPath = p;

    if (m_type == 2) {
        emit pathReceived(t, p, sender);
        return;
    }

    if (m_type == 1) {
        if (t != "BW" && t != "GRAY") return;
    } else if (m_type == 0) {
        if (t != "RGB") return;
    }

    const QString senderIp = extractSenderIp(sender);
    const QString winPath = mapDevicePathToWindowsShare(p, senderIp);

    qint64 stableSize = 0;
    if (!waitForReadableFile(winPath, 2000, &stableSize)) {
        ++m_totalReadFails;
        emit logRequested("读取失败",
                          QString("%1 文件未就绪: %2 (from=%3)").arg(t, winPath, senderIp),
                          "#F44336");
        return;
    }

    QImage loaded;
    QString decErr;
    const bool preferGray = (m_type == 1);
    readImageWithRetry(winPath, preferGray, 1200, &loaded, &decErr);
    if (loaded.isNull()) {
        ++m_totalReadFails;
        emit logRequested("读取失败",
                          QString("%1 解码失败: %2 err=%3").arg(t, winPath, decErr),
                          "#F44336");
        return;
    }

    if (m_type == 1) {
        QImage bwFull = loaded.convertToFormat(QImage::Format_Indexed8, grayColorTable());
        if (bwFull.isNull()) {
            ++m_totalReadFails;
            emit logRequested("读取失败",
                              QString("BW 转灰失败: %1").arg(winPath),
                              "#F44336");
            return;
        }
        bwFull.setColorTable(grayColorTable());

        if (m_fullSliceW <= 0 || m_fullSliceH <= 0) {
            m_fullSliceW = bwFull.width();
            m_fullSliceH = bwFull.height();
        }
        const int segments = (m_fullSliceW > 0) ? qMax(1, m_panorama.width() / m_fullSliceW) : 1;
        const uint32_t imgIdx = m_pathBwFrameIndex++;

        if (!m_panorama.isNull() && m_fullSliceW > 0) {
            const int panoW = m_panorama.width();
            const int panoH = m_panorama.height();
            const int sliceW = m_fullSliceW;

            const int tileIndex = (int)(imgIdx % (uint32_t)segments);
            const int alignedIndex = (tileIndex + (segments / 2)) % segments;
            const int startX = alignedIndex * sliceW;
            QImage src = bwFull;
            if (src.width() != sliceW || src.height() != panoH) {
                src = src.scaled(sliceW, panoH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                src = src.convertToFormat(QImage::Format_Indexed8, grayColorTable());
                src.setColorTable(grayColorTable());
            }

            const int rightW = qMin(sliceW, panoW - startX);
            const int leftW = sliceW - rightW;
            QReadWriteLock *lockA = nullptr;
            QReadWriteLock *lockB = nullptr;
            if (!m_segLocks.isEmpty()) {
                lockA = m_segLocks[bucketIndex(alignedIndex, m_segLocks.size())];
                if (leftW > 0) lockB = m_segLocks[bucketIndex(0, m_segLocks.size())];
            }
            lockTwoWrite(lockA, lockB);
            for (int y = 0; y < panoH; ++y) {
                const uchar *srcLine = src.constScanLine(y);
                uchar *dstLine = m_panorama.scanLine(y);
                memcpy(dstLine + startX, srcLine, rightW);
                if (leftW > 0) {
                    memcpy(dstLine, srcLine + rightW, leftW);
                }
            }
            unlockTwoWrite(lockA, lockB);
        }

        const int uiPanoW = 8192;
        const int previewW = qMax(1, uiPanoW / segments);
        QImage preview = bwFull.scaled(previewW, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        preview = preview.convertToFormat(QImage::Format_Indexed8, grayColorTable());
        preview.setColorTable(grayColorTable());
        const double angleDeg = (double)((imgIdx + (uint32_t)(segments / 2)) % (uint32_t)segments) * 360.0 / (double)segments;
        emit thermalFrameCaptured(preview.copy(), angleDeg);
        ++m_totalDecodedFrames;
        return;
    }

    QImage rgbFull = loaded.convertToFormat(QImage::Format_RGB32);
    if (rgbFull.isNull()) {
        ++m_totalReadFails;
        emit logRequested("读取失败",
                          QString("RGB 转色失败: %1").arg(winPath),
                          "#F44336");
        return;
    }

    if (m_fullSliceW <= 0 || m_fullSliceH <= 0) {
        m_fullSliceW = rgbFull.width();
        m_fullSliceH = rgbFull.height();
    }
    const int segments = (m_fullSliceW > 0) ? qMax(1, m_panorama.width() / m_fullSliceW) : 1;
    const uint32_t imgIdx = m_pathRgbFrameIndex++;

    if (!m_panorama.isNull() && m_fullSliceW > 0) {
        const int panoW = m_panorama.width();
        const int panoH = m_panorama.height();
        const int sliceW = m_fullSliceW;
        const int tileIndex = (int)(imgIdx % (uint32_t)segments);
        const int startX = tileIndex * sliceW;

        QImage src = rgbFull;
        if (src.width() != sliceW || src.height() != panoH) {
            src = src.scaled(sliceW, panoH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            src = src.convertToFormat(QImage::Format_RGB32);
        }

        const int rightW = qMin(sliceW, panoW - startX);
        const int leftW = sliceW - rightW;
        QReadWriteLock *lockA = nullptr;
        QReadWriteLock *lockB = nullptr;
        if (!m_segLocks.isEmpty()) {
            lockA = m_segLocks[bucketIndex(tileIndex, m_segLocks.size())];
            if (leftW > 0) lockB = m_segLocks[bucketIndex(0, m_segLocks.size())];
        }
        lockTwoWrite(lockA, lockB);
        for (int y = 0; y < panoH; ++y) {
            const uchar *srcLine = src.constScanLine(y);
            uchar *dstLine = m_panorama.scanLine(y);
            memcpy(dstLine + startX * 4, srcLine, rightW * 4);
            if (leftW > 0) {
                memcpy(dstLine, srcLine + rightW * 4, leftW * 4);
            }
        }
        unlockTwoWrite(lockA, lockB);
    }

    const int uiPanoW = 8192;
    const int previewW = qMax(1, uiPanoW / segments);
    QImage preview = rgbFull.scaled(previewW, 240, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const double angleDeg = (double)(imgIdx % (uint32_t)segments) * 360.0 / (double)segments;
    emit frameCaptured(preview.copy(), angleDeg);
    ++m_totalDecodedFrames;
}

void VideoThread::onWorkerLogRequested(const QString &type, const QString &msg, const QString &color)
{
    emit logRequested(type, msg, color);
}
