#ifndef TARGETINFOPANEL_H
#define TARGETINFOPANEL_H

#include "targetrecord.h"

#include <QFormLayout>
#include <QLabel>
#include <QWidget>

class TargetInfoPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TargetInfoPanel(QWidget *parent = nullptr);
    void setTarget(const TargetRecord &target);
    void clearTarget();

private:
    QLabel *m_id = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_azimuth = nullptr;
    QLabel *m_pano = nullptr;
#ifdef DMX_TEST_BUILD
    QLabel *m_verticalFov = nullptr;
    QLabel *m_elevation = nullptr;
#endif
    QLabel *m_frame = nullptr;
    QLabel *m_stream = nullptr;
#ifdef DMX_ADVANCED_DETECTION
    QLabel *m_class = nullptr;
#endif
    QLabel *m_confidence = nullptr;
    QLabel *m_state = nullptr;
};

#endif // TARGETINFOPANEL_H
