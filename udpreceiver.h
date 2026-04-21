#ifndef UDPRECEIVER_H
#define UDPRECEIVER_H

#include <QObject>
#include <QUdpSocket>
#include <QByteArray>
#include <QImage>
#include <QMap>
#include "udpprotocol.h"

class UdpReceiver : public QObject
{
    Q_OBJECT
public:
    explicit UdpReceiver(QObject *parent = nullptr);
    ~UdpReceiver();

    void startListening();

signals:
    // Y型数据流 - 热路：直接把解码好的图和它的全局序号发给主界面用于全景平铺
    void liveFrameReady(QImage img, int imageIndex, int type);

    // Y型数据流 - 冷路：把收到的文件路径发给主界面，存入零缓存页表
    void pageTablePathReceived(QString path, int type);

private slots:
    void readPathDatagrams(); // 处理 8001 端口的消息
    void readColorDatagrams(); // 处理 8003 端口的彩色图像
    void readThermalDatagrams(); // 处理 8002 端口的黑白图像

private:
    void processImageChunk(const QByteArray &datagram, int type);

    QUdpSocket *m_socket8001; // 文件路径消息端口
    QUdpSocket *m_socket8002; // 黑白压缩图像数据端口
    QUdpSocket *m_socket8003; // 彩色压缩图像数据端口

    // 组包缓冲池 (Jitter Buffer)：根据 imageIndex 拼装对应的二进制块
    struct ImageBuffer {
        uint32_t totalSize = 0;
        int receivedBytes = 0;
        QByteArray data;
    };
    QMap<uint32_t, ImageBuffer> m_colorBuffer;
    QMap<uint32_t, ImageBuffer> m_thermalBuffer;
};

#endif // UDPRECEIVER_H
