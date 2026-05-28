#include "panoramawidget.h"
#include <QPainter>
#include <QMouseEvent>

static int rulerH(bool show) { return show ? 16 : 0; }

PanoramaWidget::PanoramaWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(80);
    m_selectedAngle = -1.0;
    m_roiPixelWidth = 100;
}

void PanoramaWidget::setShowRuler(bool show)
{
    if (m_showRuler == show) return;
    m_showRuler = show;
    update();
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
    const int rulerHeight = rulerH(m_showRuler);
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

double PanoramaWidget::angleFromX(int x) const
{
    const int w = width();
    if (w <= 0) return 0.0;
    double ratio = (double)x / (double)w;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    double angle = ratio * 360.0;
    if (angle < 0.0) angle = 0.0;
    if (angle >= 360.0) angle = 359.9;
    return angle;
}

void PanoramaWidget::selectAngleFromX(int x)
{
    if (m_image.isNull()) return;
    m_selectedAngle = angleFromX(x);
    update();
    emit angleSelected(m_selectedAngle);
}

void PanoramaWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_image.isNull()) return;
    if (event->button() != Qt::LeftButton) return;
    m_dragging = true;
    selectAngleFromX(event->x());
}

void PanoramaWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    if (!(event->buttons() & Qt::LeftButton)) return;
    selectAngleFromX(event->x());
}

void PanoramaWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    m_dragging = false;
}

void PanoramaWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);

    painter.fillRect(rect(), Qt::black);

    const int rulerHeight = rulerH(m_showRuler);
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

    if (!m_showRuler) return;

    QFont rulerFont = painter.font();
    rulerFont.setPointSize(7);
    painter.setFont(rulerFont);

    painter.setPen(QColor(180, 180, 180));
    painter.drawLine(0, height() - rulerHeight, width(), height() - rulerHeight);

    const int step = 45;
    const int smallStep = 15;

    for (int deg = 0; deg <= 360; deg += smallStep) {
        const double r = deg / 360.0;
        const int x = (int)(r * width());

        const int lineBottom = height();
        int lineTop = height() - 3;

        if (deg % step == 0) {
            lineTop = height() - 7;
            if (deg < 360) {
                const QString text = QString::number(deg);
                const int textWidth = painter.fontMetrics().width(text);
                painter.drawText(x - textWidth / 2, height() - 8, text);
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
