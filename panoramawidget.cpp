#include "panoramawidget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QDebug>

PanoramaWidget::PanoramaWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(250); // 稍微加高一点，留出底部画标尺的空间
    m_selectedAngle = -1.0;
    m_roiPixelWidth = 100; // 默认ROI框宽度，可根据摄像头视场角调整
}

void PanoramaWidget::updateImage(const QImage &img)
{
    m_image = img;
    update();
}

void PanoramaWidget::setSelectionBoxWidth(int width)
{
    m_roiPixelWidth = width;
}

void PanoramaWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_image.isNull()) return;

    // 1. 计算点击位置对应的精确角度 (0 ~ 360)
    double ratio = (double)event->x() / width();
    double angle = ratio * 360.0;

    // 2. 限制范围
    if (angle < 0) angle = 0;
    if (angle >= 360) angle = 359.9;

    m_selectedAngle = angle;

    // 3. 触发重绘（画选中框）并发送信号
    update();
    emit angleSelected(m_selectedAngle);

    qDebug() << "全景图选中角度:" << m_selectedAngle;
}

void PanoramaWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);

    // 背景黑底
    painter.fillRect(rect(), Qt::black);

    // 1. 绘制全景底图 (留出底部 30像素 画标尺)
    int rulerHeight = 30;
    QRect imageRect(0, 0, width(), height() - rulerHeight);

    if (!m_image.isNull()) {
        painter.drawImage(imageRect, m_image);
    }

    // 2. 绘制选中区域 (绿色方框)
    if (m_selectedAngle >= 0) {
        double ratio = m_selectedAngle / 360.0;
        int centerX = (int)(ratio * width());
        int leftX = centerX - (m_roiPixelWidth / 2);

        painter.setPen(QPen(Qt::green, 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(leftX, 0, m_roiPixelWidth, imageRect.height());
    }

    // 3. 绘制底部标尺
    painter.setPen(Qt::white);
    painter.drawLine(0, height() - rulerHeight, width(), height() - rulerHeight); // 横轴线

    int step = 45; // 大刻度间隔 (度)
    int smallStep = 15; // 小刻度

    for (int deg = 0; deg <= 360; deg += smallStep) {
        // 计算X坐标
        double r = deg / 360.0;
        int x = (int)(r * width());

        int lineBottom = height();
        int lineTop = height() - 5; // 小刻度长度

        if (deg % step == 0) {
            // 大刻度
            lineTop = height() - 10;
            // 绘制文字 (0, 45, 90...)
            QString text = QString::number(deg);
            int textWidth = painter.fontMetrics().width(text);
            if (deg < 360) { // 360度不画，防止重叠
                 painter.drawText(x - textWidth/2, height() - 12, text);
            }
        }

        painter.drawLine(x, lineTop, x, lineBottom);
    }
}
// 【新增】：外部遥控设置方框位置
void PanoramaWidget::setSelectedAngle(double angle)
{
    // 💡 注意：这里的 m_selectedAngle 是假设您内部用来记录方框角度的变量名。
    // 如果您用的变量名叫 m_angle 或 currentAngle，请将下面这行替换为您真实的变量名！
    m_selectedAngle = angle;

    // 强制触发 paintEvent 重绘，把方框画到新位置
    update();
}
