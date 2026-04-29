#include "udpreceiver.h"
#include <QDebug>
#include <QStringList>
#include <QDateTime>
#include <opencv2/opencv.hpp>

UdpReceiver::UdpReceiver(QObject *parent) : QObject(parent)
{
    m_socket8001 = new QUdpSocket(this);
    m_socket8002 = new QUdpSocket(this);
    m_socket8003 = new QUdpSocket(this);

    connect(m_socket8001, &QUdpSocket::readyRead, this, &UdpReceiver::readPathDatagrams);
    connect(m_socket8002, &QUdpSocket::readyRead, this, &UdpReceiver::readThermalDatagrams);
    connect(m_socket8003, &QUdpSocket::readyRead, this, &UdpReceiver::readColorDatagrams);
}

UdpReceiver::~UdpReceiver() {}

void UdpReceiver::startListening()
{
    // 绑定端口开始监听
    m_socket8001->bind(QHostAddress::Any, 8001);
    m_socket8002->bind(QHostAddress::Any, 8002);
    m_socket8003->bind(QHostAddress::Any, 8003);
    qDebug() << "UDP 监听已启动：端口 8001(路径), 8002(黑白), 8003(彩色)";
}

// ==========================================
// 1. 处理端口 8001 的文件路径消息
// ==========================================
void UdpReceiver::readPathDatagrams()
{
    while (m_socket8001->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_socket8001->pendingDatagramSize());
        m_socket8001->readDatagram(datagram.data(), datagram.size());

        // 协议示例：RGB; \\192.168.4.1\data\raw\rgb\20260315\1000\rgb_20260315_100001_01.jpg [cite: 17]
        QString msg = QString::fromUtf8(datagram).trimmed();
        QStringList parts = msg.split(";");
        if (parts.size() == 2) {
            QString typeStr = parts[0].trimmed();
            QString path = parts[1].trimmed();
            int type = (typeStr == "RGB") ? 1 : 0;

            // 直接触发页表更新信号 (冷路)
            emit pageTablePathReceived(path, type);

            // 可选：在这里调用 OpenCV 读取 path 并触发 liveFrameReady (热路)
        }
    }
}

// ==========================================
// 2. 处理端口 8002/8003 的裸流组包
// ==========================================
void UdpReceiver::readColorDatagrams()
{
    while (m_socket8003->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_socket8003->pendingDatagramSize());
        m_socket8003->readDatagram(datagram.data(), datagram.size());
        processImageChunk(datagram, 1);
    }
}

void UdpReceiver::readThermalDatagrams()
{
    while (m_socket8002->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_socket8002->pendingDatagramSize());
        m_socket8002->readDatagram(datagram.data(), datagram.size());
        processImageChunk(datagram, 0);
    }
}

void UdpReceiver::processImageChunk(const QByteArray &datagram, int type)
{
    if (datagram.size() < sizeof(ImagePacketHeader)) return;

    // 解析 20 字节包头 [cite: 20]
    const ImagePacketHeader *header = reinterpret_cast<const ImagePacketHeader*>(datagram.constData());

    // 校验固定码 0xFFFF [cite: 20]
    if (header->headerCode != 0xFFFF) return;

    QMap<uint32_t, ImageBuffer> &bufferPool = (type == 1) ? m_colorBuffer : m_thermalBuffer;
    uint32_t &maxSeen = (type == 1) ? m_colorMaxIndexSeen : m_thermalMaxIndexSeen;
    quint64 &rxCounter = (type == 1) ? m_colorRxCounter : m_thermalRxCounter;

    uint32_t imgIdx = header->imageIndex;
    int payloadSize = datagram.size() - sizeof(ImagePacketHeader);
    const char *payloadData = datagram.constData() + sizeof(ImagePacketHeader);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    ++rxCounter;

    if (maxSeen != 0 && imgIdx + 1000U < maxSeen) {
        bufferPool.clear();
        maxSeen = imgIdx;
    } else if (imgIdx > maxSeen) {
        maxSeen = imgIdx;
    }

    ImageBuffer &buf = bufferPool[imgIdx];
    if (buf.totalSize == 0) {
        buf.totalSize = header->totalSize;
        buf.data.reserve((int)header->totalSize);
        buf.createdMs = nowMs;
        buf.lastUpdateMs = nowMs;
        buf.lastProgressBytes = 0;
    }

    if (payloadSize > 0) {
        buf.data.append(payloadData, payloadSize);
        buf.receivedBytes += static_cast<uint32_t>(payloadSize);
        buf.lastUpdateMs = nowMs;
    }

    if (rxCounter % 64 == 0 || bufferPool.size() > 128) {
        const uint32_t window = 30;
        const uint32_t cutoff = (maxSeen > window) ? (maxSeen - window) : 0;
        auto it = bufferPool.begin();
        while (it != bufferPool.end() && it.key() < cutoff) {
            it = bufferPool.erase(it);
        }
        const qint64 stallMs = 1500;
        const qint64 hardMs = 5000;
        it = bufferPool.begin();
        while (it != bufferPool.end()) {
            ImageBuffer &b = it.value();
            const bool noProgress = (b.receivedBytes == b.lastProgressBytes);
            if (noProgress) {
                if ((b.lastUpdateMs > 0 && nowMs - b.lastUpdateMs > stallMs) ||
                    (b.createdMs > 0 && nowMs - b.createdMs > hardMs)) {
                    it = bufferPool.erase(it);
                    continue;
                }
            } else {
                b.lastProgressBytes = b.receivedBytes;
            }
            ++it;
        }
    }

    // 判断是否接收完一张完整的 JPG
    if (buf.totalSize > 0 && buf.receivedBytes >= buf.totalSize) {
        // 解码完整 JPG
        std::vector<uchar> bytes(buf.data.begin(), buf.data.end());
        cv::Mat mat = cv::imdecode(bytes, type == 1 ? cv::IMREAD_COLOR : cv::IMREAD_GRAYSCALE);

        if (!mat.empty()) {
            // 【结合您的新逻辑】：不再切狭缝，直接整张图降维平铺！
            cv::resize(mat, mat, cv::Size(mat.cols, 240), 0, 0, cv::INTER_AREA);
            QImage img;
            if (type == 1) {
                cv::cvtColor(mat, mat, cv::COLOR_BGR2BGRA);
                img = QImage((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_RGB32).copy();
            } else {
                img = QImage((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Grayscale8).copy();
            }

            // 抛给主界面更新全景图
            emit liveFrameReady(img, imgIdx, type);
        }

        // 释放已完成的缓冲池
        bufferPool.remove(imgIdx);
    }
}
