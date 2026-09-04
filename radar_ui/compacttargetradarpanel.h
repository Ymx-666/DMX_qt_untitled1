#ifndef COMPACTTARGETRADARPANEL_H
#define COMPACTTARGETRADARPANEL_H

#include "targetrecord.h"

#include <QImage>
#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;
class QResizeEvent;
class TargetRadarWidget;

class CompactTargetRadarPanel : public QWidget
{
    Q_OBJECT
public:
    explicit CompactTargetRadarPanel(QWidget *parent = nullptr);

    void setPanorama(const QImage &panorama, bool bwNeeds180Align = false);
    void setTargets(const QVector<TargetRecord> &targets);
    void setScanAngle(double angleDeg);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateTargetDetails();
    void rebuildTargetList();
    void selectTarget(int index);
    void updatePreview();
    void updatePreviewPixmap();

    TargetRadarWidget *m_radar = nullptr;
    QLabel *m_details = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_previewDetails = nullptr;
    QVector<TargetRecord> m_targets;
    QImage m_panorama;
    QImage m_previewImage;
    int m_selected = -1;
};

#endif // COMPACTTARGETRADARPANEL_H
