#ifndef PANORAMAWIDGET_H
#define PANORAMAWIDGET_H

#include <QImage>
#include <QRect>
#include <QWidget>

class PanoramaWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PanoramaWidget(QWidget *parent = nullptr);
    void updateImage(const QImage &img);
    void updateImagePartial(const QImage &img, const QRect &dirtyInImageCoords);
    void setSelectionBoxWidth(int width);
    void setSelectedAngle(double angle);
    void setShowRuler(bool show);

signals:
    void angleSelected(double angle);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    double angleFromX(int x) const;
    void selectAngleFromX(int x);

    QImage m_image;
    double m_selectedAngle = -1.0;
    int m_roiPixelWidth = 100;
    bool m_showRuler = true;
    bool m_dragging = false;
};

#endif
