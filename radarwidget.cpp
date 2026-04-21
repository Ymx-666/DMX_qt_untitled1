#include "radarwidget.h"
#include <QPainter>
#include <QtMath>
#include <QMouseEvent>
#include <QConicalGradient>

RadarWidget::RadarWidget(QWidget *parent) : QWidget(parent)
{
    QPalette pal = palette();
    pal.setColor(QPalette::Background, Qt::black);
    setAutoFillBackground(true);
    setPalette(pal);
    currentAngle = 0;
    setMinimumSize(200, 200);

    // [新增] 初始化闪烁定时器 (500毫秒闪一次)
    m_blinkState = true;
    blinkTimer = new QTimer(this);
    connect(blinkTimer, &QTimer::timeout, this, [=](){
        m_blinkState = !m_blinkState; // 切换亮灭状态
        update(); // 触发重绘
    });
    blinkTimer->start(500); // 启动定时器
}

void RadarWidget::setCurrentAngle(int angle)
{
    currentAngle = angle;
    update();
}

void RadarWidget::setTargets(const QVector<RadarTarget>& targets)
{
    m_targets = targets;
    update();
}

void RadarWidget::mousePressEvent(QMouseEvent *event)
{
    double dx = event->x() - width() / 2;
    double dy = height() / 2 - event->y();
    double angle = qAtan2(dx, dy) * 180 / M_PI;
    if (angle < 0) angle += 360;
    emit sectorClicked((int)angle);
}

void RadarWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int side = qMin(width(), height());
    QPoint center(width() / 2, height() / 2);
    int radius = (side / 2) - 25;

    // 1. 绘制雷达底盘 (深绿色网格)
    painter.setPen(QPen(QColor(0, 100, 0), 1));
    painter.drawEllipse(center, radius, radius);
    painter.drawEllipse(center, radius * 2 / 3, radius * 2 / 3);
    painter.drawEllipse(center, radius / 3, radius / 3);
    painter.drawLine(center.x() - radius, center.y(), center.x() + radius, center.y());
    painter.drawLine(center.x(), center.y() - radius, center.x(), center.y() + radius);

    // 2. 绘制扫描线
    double rad = qDegreesToRadians((double)currentAngle);
    int endX = center.x() + radius * qSin(rad);
    int endY = center.y() - radius * qCos(rad);

    QConicalGradient gradient(center, currentAngle + 180);
    gradient.setColorAt(0.5, QColor(0, 255, 0, 180));
    gradient.setColorAt(1.0, QColor(0, 255, 0, 0));
    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);
    painter.drawPie(center.x() - radius, center.y() - radius, radius*2, radius*2, (90 - currentAngle) * 16, 60 * 16);

    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(center, QPoint(endX, endY));

    // ================= [核心修改] 3. 绘制目标点 (仅已探测的) =================
    painter.setPen(Qt::NoPen);

    for(const auto& target : m_targets) {
        // [关键] 如果没被探测到，直接跳过不画 (隐身)
        if (!target.isDetected) continue;

        double tRad = qDegreesToRadians((double)target.angle);
        int tx = center.x() + (radius * 0.8) * qSin(tRad); // 距离圆心 80% 处
        int ty = center.y() - (radius * 0.8) * qCos(tRad);

        // [关键] 闪烁逻辑
        if (m_blinkState) {
            // 亮状态：画大的红色光晕 + 亮红核心
            painter.setBrush(QColor(255, 0, 0, 100)); // 半透明光晕
            painter.drawEllipse(QPoint(tx, ty), 12, 12);
            painter.setBrush(Qt::red); // 实心红点
            painter.drawEllipse(QPoint(tx, ty), 6, 6);
        } else {
            // 灭状态：只画一个小一点的暗红点，或者完全不画模拟闪烁
            // 这里选择画暗红，模拟 LED 闪烁的余光
            painter.setBrush(QColor(100, 0, 0));
            painter.drawEllipse(QPoint(tx, ty), 6, 6);
        }
    }
    // =====================================================================

    // 4. 绘制方位文字
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setPointSize(9);
    painter.setFont(font);
    struct Label { int ang; QString txt; };
    QVector<Label> labels = { {0, "N"}, {90, "E"}, {180, "S"}, {270, "W"}};
    int textRadius = radius + 15;
    for(const auto &lbl : labels) {
        double r = qDegreesToRadians((double)lbl.ang);
        int tx = center.x() + textRadius * qSin(r);
        int ty = center.y() - textRadius * qCos(r);
        int tw = painter.fontMetrics().width(lbl.txt);
        int th = painter.fontMetrics().height();
        painter.drawText(tx - tw/2, ty + th/4, lbl.txt);
    }
}
