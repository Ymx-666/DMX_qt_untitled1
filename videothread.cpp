#include "videothread.h"
#include <QDebug>
#include <QVector>
#include <QStringList>
#include <QFileInfo>
#include <QTime>
#include <opencv2/opencv.hpp>

VideoThread::VideoThread(int type, QObject *parent)
    : QThread(parent), m_type(type), m_running(false), m_dataSocket(nullptr), m_pathSocket(nullptr)
{
    qDebug() << "UDP 监听线程已就绪，类型:" << (m_type == 0 ? "彩色(兼管8001路径总机)" : "黑白(纯8002裸流)");
}

VideoThread::~VideoThread()
{
    stop();
    wait();
}

void VideoThread::stop()
{
    m_running = false;
    quit();
}

void VideoThread::run()
{
    m_running = true;

    // 1. 各自监听自己的裸流数据端口 (8002 和 8003)
    m_dataSocket = new QUdpSocket();
    int dataPort = (m_type == 0) ? 8003 : 8002;
    // 【关键修复 1】：强制使用 AnyIPv4，避免 Linux 默认绑定 IPv6 导致无法接收 IPv4 报文
    m_dataSocket->bind(QHostAddress::AnyIPv4, dataPort, QUdpSocket::ShareAddress);
    connect(m_dataSocket, SIGNAL(readyRead()), this, SLOT(processPendingDatagrams()));

    // 2. 彩色线程(m_type==0)独占 8001 端口，作为“总机调度员”
    if (m_type == 0) {
        m_pathSocket = new QUdpSocket();
        // 【关键修复 2】：强制使用 AnyIPv4 严格对齐物理机/脚本的 0.0.0.0
        if (m_pathSocket->bind(QHostAddress::AnyIPv4, 8001, QUdpSocket::ShareAddress)) {
            emit logRequested("系统", "成功绑定 8001 端口 (IPv4)", "#00AAAA");
        } else {
            emit logRequested("系统", "错误：8001 端口绑定失败！请确保 Python 测试脚本已关闭！", "#F44336");
        }
        connect(m_pathSocket, SIGNAL(readyRead()), this, SLOT(processPathDatagrams()));
    }

    exec();

    delete m_dataSocket;
    if (m_pathSocket) delete m_pathSocket;
}

// 裸流二进制组包引擎 (处理 8002/8003)
void VideoThread::processPendingDatagrams()
{
    while (m_dataSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_dataSocket->pendingDatagramSize());
        m_dataSocket->readDatagram(datagram.data(), datagram.size());

        if (datagram.size() < (int)sizeof(ImagePacketHeader)) continue;

        const ImagePacketHeader *header = reinterpret_cast<const ImagePacketHeader*>(datagram.constData());
        if (header->headerCode != 0xFFFF) continue;

        uint32_t imgIdx = header->imageIndex;
        int payloadSize = datagram.size() - sizeof(ImagePacketHeader);
        const char *payloadData = datagram.constData() + sizeof(ImagePacketHeader);

        if (!m_bufferPool.contains(imgIdx)) {
            m_bufferPool[imgIdx].totalSize = header->totalSize;
            m_bufferPool[imgIdx].data.reserve(header->totalSize);
        }

        m_bufferPool[imgIdx].data.append(payloadData, payloadSize);
        m_bufferPool[imgIdx].receivedBytes += static_cast<uint32_t>(payloadSize);

        if (m_bufferPool[imgIdx].receivedBytes >= m_bufferPool[imgIdx].totalSize) {
            std::vector<uchar> buf(m_bufferPool[imgIdx].data.begin(), m_bufferPool[imgIdx].data.end());
            cv::Mat frame = cv::imdecode(buf, m_type == 1 ? cv::IMREAD_GRAYSCALE : cv::IMREAD_COLOR);

            if (!frame.empty()) {
                // 【注意】：严禁在此处使用 cv::resize，保留原始高分辨率数据传给 MainWindow 的内存画板
                QImage img;
                if (m_type == 1) {
                    img = QImage((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_Indexed8);
                    static QVector<QRgb> s_grayTable;
                    if (s_grayTable.isEmpty()) {
                        for (int c = 0; c < 256; ++c) s_grayTable.push_back(qRgb(c, c, c));
                    }
                    img.setColorTable(s_grayTable);
                    emit frameCaptured(img.copy(), imgIdx);
                } else {
                    cv::cvtColor(frame, frame, cv::COLOR_BGR2BGRA);
                    img = QImage((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_RGB32);
                    emit frameCaptured(img.copy(), imgIdx);
                }
            }
            m_bufferPool.remove(imgIdx);
        }
    }
}

// 8001 路径总机：根据 RGB 和 BW 分发数据
void VideoThread::processPathDatagrams()
{
    while (m_pathSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_pathSocket->pendingDatagramSize());
        m_pathSocket->readDatagram(datagram.data(), datagram.size());

        QString msg = QString::fromUtf8(datagram).trimmed();

        // 收到数据立即发送给日志侧拉框，用于实时监控硬件发包情况
        emit logRequested("门址 (8001)", "RAW: " + msg, "#00AAAA");

        QStringList parts = msg.split(";");

        if (parts.size() >= 2) {
            // 【协议兼容】：强制转大写，处理 rgb -> RGB
            QString typeStr = parts[0].trimmed().toUpper();
            QString originalPath = parts[1].trimmed();

            // 【核心修复】：Samba共享文件夹路径映射逻辑
            // 硬件发来的路径类似于: /data/raw/20260418/RGB/1636/img.jpg
            // 你的本地挂载点是:   /home/ymx/untitled1/data

            QString linuxPath = originalPath;

            if (linuxPath.startsWith("/data/")) {
                // 将开头的 "/data/" 替换为你本地的实际挂载根目录 "/home/ymx/untitled1/data/"
                // 这样中间的 "raw/20260418/RGB/1636/" 目录结构就会被完美保留
                linuxPath.replace(0, 6, "/home/ymx/untitled1/data/");
            } else {
                // 防御性编程：如果硬件发来的格式不规范，退回到只取文件名的老逻辑
                QString filename = originalPath.contains("/") ? originalPath.section('/', -1) : originalPath.section('\\', -1);
                linuxPath = "/home/ymx/untitled1/data/" + filename;
            }

            // 发送给主界面记录（用于后续 ROI 截取）
            emit pageTablePathReceived(linuxPath);

            cv::Mat frame = cv::imread(linuxPath.toStdString(), cv::IMREAD_COLOR);

            if (!frame.empty()) {
                // 【注意】：严禁在此处使用 cv::resize，保留原始高分辨率数据传给 MainWindow 的内存画板
                static int rgb_index = 0;
                static int bw_index = 0;

                // 根据协议，RGB发给彩色画布，BW发给黑白画布
                if (typeStr == "RGB") {
                    cv::cvtColor(frame, frame, cv::COLOR_BGR2BGRA);
                    QImage img = QImage((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_RGB32);
                    emit frameCaptured(img.copy(), rgb_index++);
                } else if (typeStr == "BW" || typeStr == "GRAY") {
                    cv::cvtColor(frame, frame, cv::COLOR_BGR2GRAY);
                    QImage img = QImage((const uchar*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_Indexed8);
                    static QVector<QRgb> s_grayTable;
                    if (s_grayTable.isEmpty()) {
                        for (int c = 0; c < 256; ++c) s_grayTable.push_back(qRgb(c, c, c));
                    }
                    img.setColorTable(s_grayTable);
                    emit thermalFrameCaptured(img.copy(), bw_index++);
                }
            } else {
                // 如果读取失败，在侧拉框打印红色报错，包含最终拼接出的绝对路径
                emit logRequested("读取失败", "文件不存在或无权限: " + linuxPath, "#F44336");
            }
        }
    }
}
