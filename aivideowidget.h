#ifndef AIVIDEOWIDGET_H
#define AIVIDEOWIDGET_H

#include <QWidget>
#include <QImage>

class AIVideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit AIVideoWidget(QWidget *parent = nullptr);
    void updateImage(const QImage &img); // 声明此函数

signals:
    void clickedAt(QPoint pos);         // 声明信号

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QImage currentImage;                // 声明变量，解决截图1报错
};

#endif
