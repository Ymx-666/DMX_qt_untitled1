#include "aivideowidget.h"
#include <QPainter>
#include <QMouseEvent>

AIVideoWidget::AIVideoWidget(QWidget *parent) : QWidget(parent) {
    setMinimumSize(100, 100);
}

void AIVideoWidget::updateImage(const QImage &img) {
    currentImage = img;
    update(); // 刷新界面
}

void AIVideoWidget::mousePressEvent(QMouseEvent *event) {
    // 发出点击坐标信号
    emit clickedAt(event->pos());
}

void AIVideoWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);

    if (currentImage.isNull()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }

    // 将图片拉伸绘制到整个控件
    painter.drawImage(rect(), currentImage);
}
