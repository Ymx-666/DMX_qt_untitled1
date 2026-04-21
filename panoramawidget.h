#ifndef PANORAMAWIDGET_H
#define PANORAMAWIDGET_H

#include <QWidget>
#include <QImage>

class PanoramaWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PanoramaWidget(QWidget *parent = nullptr);
    void updateImage(const QImage &img); // 更新全景底图
    void setSelectionBoxWidth(int width); // 设置ROI区域对应的宽度（例如视场角宽度）
    void setSelectedAngle(double angle);

signals:
    // 发出精确的角度信号 (0.0 - 360.0)
    void angleSelected(double angle);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QImage m_image;
    double m_selectedAngle; // 当前选中的角度 (-1 表示未选中)
    int m_roiPixelWidth;    // 选中的ROI区域在画面上对应的像素宽度
};

#endif // PANORAMAWIDGET_H
