#ifndef RADARWIDGET_H
#define RADARWIDGET_H

#include <QWidget>
#include <QVector>
#include <QTimer> // [新增]

// 目标结构体 (保持不变)
struct RadarTarget {
    int angle;
    bool isDetected;
    RadarTarget() : angle(0), isDetected(false) {}
    RadarTarget(int a) : angle(a), isDetected(false) {}
};

class RadarWidget : public QWidget
{
    Q_OBJECT
public:
    explicit RadarWidget(QWidget *parent = nullptr);

    void setCurrentAngle(int angle);
    void setTargets(const QVector<RadarTarget>& targets);

signals:
    void sectorClicked(int angle);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int currentAngle;
    QVector<RadarTarget> m_targets;

    // [新增] 用于控制报警闪烁的定时器
    QTimer *blinkTimer;
    bool m_blinkState; // 闪烁状态 (亮/灭)
};

#endif // RADARWIDGET_H
