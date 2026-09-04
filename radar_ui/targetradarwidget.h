#ifndef TARGETRADARWIDGET_H
#define TARGETRADARWIDGET_H

#include "targetrecord.h"

#include <QImage>
#include <QWidget>
#include <QVector>

class QTimer;

class TargetRadarWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TargetRadarWidget(QWidget *parent = nullptr);

    void setPanoramaBackground(const QImage &panorama, bool bwNeeds180Align);
    void setCompactMode(bool compact);
    void setTargets(const QVector<TargetRecord> &targets);
    void setSelectedTarget(int index);
    void setScanAngle(double angleDeg);
    void setNorthOffsetDeg(double rawNorthDeg);
    double northOffsetDeg() const;

signals:
    void targetClicked(int index);
    void azimuthClicked(double angleDeg);
    void northOffsetChanged(double rawNorthDeg);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildPolar();
    QRect radarRect() const;
    double targetRadius(const TargetRecord &target) const;
    QPointF targetPoint(const TargetRecord &target) const;
    QPointF polarPoint(double angleDeg, double normalizedRadius) const;
    double rawToDisplayAngle(double rawAngle) const;
    double displayToRawAngle(double displayAngle) const;
    double pointDisplayAzimuth(const QPointF &p) const;
    double pointAzimuth(const QPointF &p) const;
    void advanceScanAnimation();

    QImage m_panorama;
    QImage m_polar;
    QVector<TargetRecord> m_targets;
    int m_selected = -1;
    double m_displayScanAngle = 0.0;
    double m_targetScanAngle = 0.0;
    double m_rawScanAngle = 0.0;
    double m_northOffsetDeg = 0.0;
    QTimer *m_scanTimer = nullptr;
    bool m_bwNeeds180Align = false;
    bool m_compactMode = false;
};

#endif // TARGETRADARWIDGET_H
