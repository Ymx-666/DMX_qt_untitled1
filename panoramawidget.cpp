#include "panoramawidget.h"
#include <QPainter>
#include <QMouseEvent>

PanoramaWidget::PanoramaWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(250);
    m_selectedAngle = -1.0;
    m_roiPixelWidth = 100;
}

void PanoramaWidget::updateImage(const QImage &img)
{
    m_image = img;
    update();
}

void PanoramaWidget::updateImagePartial(const QImage &img, const QRect &dirtyInImageCoords)
{
    m_image = img;
    if (m_image.isNull()) {
        update();
        return;
    }
    const QRect imgBounds(0, 0, m_image.width(), m_image.height());
    const QRect dirty = dirtyInImageCoords.intersected(imgBounds);
    if (dirty.isNull()) {
        update();
        return;
    }
    const int rulerHeight = 30;
    const QRect imageRect(0, 0, width(), height() - rulerHeight);
    const qint64 iw = m_image.width();
    const qint64 ih = m_image.height();
    const qint64 ww = imageRect.width();
    const qint64 wh = imageRect.height();
    if (iw <= 0 || ih <= 0 || ww <= 0 || wh <= 0) {
        update();
        return;
    }
    const int x1 = imageRect.left() + (int)((qint64)dirty.left() * ww / iw);
    const int x2 = imageRect.left() + (int)(((qint64)dirty.right() + 1) * ww / iw) - 1;
    const int y1 = imageRect.top() + (int)((qint64)dirty.top() * wh / ih);
    const int y2 = imageRect.top() + (int)(((qint64)dirty.bottom() + 1) * wh / ih) - 1;
    update(QRect(QPoint(x1, y1), QPoint(x2, y2)));
}

void PanoramaWidget::setSelectionBoxWidth(int width)
{
    m_roiPixelWidth = width;
}

void PanoramaWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_image.isNull()) return;

    double ratio = (double)event->x() / width();
    double angle = ratio * 360.0;

    if (angle < 0) angle = 0;
    if (angle >= 360) angle = 359.9;

    m_selectedAngle = angle;

    update();
    emit angleSelected(m_selectedAngle);
}

void PanoramaWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);

    painter.fillRect(rect(), Qt::black);

    int rulerHeight = 30;
    QRect imageRect(0, 0, width(), height() - rulerHeight);

    if (!m_image.isNull()) {
        painter.drawImage(imageRect, m_image);
    }

    if (m_selectedAngle >= 0) {
        double ratio = m_selectedAngle / 360.0;
        int centerX = (int)(ratio * width());
        int leftX = centerX - (m_roiPixelWidth / 2);

        painter.setPen(QPen(Qt::green, 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(leftX, 0, m_roiPixelWidth, imageRect.height());
    }

    painter.setPen(Qt::white);
    painter.drawLine(0, height() - rulerHeight, width(), height() - rulerHeight); // 横轴线

    int step = 45; // 大刻度间隔 (度)
    int smallStep = 15; // 小刻度

    for (int deg = 0; deg <= 360; deg += smallStep) {
        double r = deg / 360.0;
        int x = (int)(r * width());

        int lineBottom = height();
        int lineTop = height() - 5; // 小刻度长度

        if (deg % step == 0) {
            lineTop = height() - 10;
            QString text = QString::number(deg);
            int textWidth = painter.fontMetrics().width(text);
            if (deg < 360) {
                 painter.drawText(x - textWidth/2, height() - 12, text);
            }
        }

        painter.drawLine(x, lineTop, x, lineBottom);
    }
}
void PanoramaWidget::setSelectedAngle(double angle)
{
    m_selectedAngle = angle;
    update();
}
