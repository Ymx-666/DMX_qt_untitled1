#ifndef TARGETRADARWINDOW_H
#define TARGETRADARWINDOW_H

#include "targetrecord.h"
#ifdef DMX_ADVANCED_DETECTION
#include "manualnegativefeedback.h"
#endif

#include <QWidget>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class TargetInfoPanel;
class TargetListWidget;
class TargetPreviewPanel;
class TargetRadarWidget;

class TargetRadarWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TargetRadarWindow(const QString &panoramaPath = QString(), QWidget *parent = nullptr);
    void setLivePanorama(const QImage &panorama, bool bwNeeds180Align);
    void setLivePanoramas(const QImage &rgbPanorama,
                          const QImage &bwPanorama,
                          bool bwNeeds180Align);
    void setScanAngle(double angleDeg);
    QString addOrUpdateTarget(const TargetRecord &target);
    void suppressStaticClutter(const QString &stream,
                               const QString &className,
                               int panoX,
                               int panoY,
                               int trackId,
                               int stableHits,
                               double contextScore);
    void clearTargets();

signals:
    void visibleTargetsChanged(const QVector<TargetRecord> &targets);

private slots:
    void selectTarget(int index);
    void openTargetImage(int index);
#ifdef DMX_ADVANCED_DETECTION
    void markFalsePositive(int sourceIndex, const QString &targetId);
#endif
    void handleAzimuth(double angleDeg);
    void changeBackgroundMode(int index);
    void applyPendingNorthOffset();
    void resetNorthOffset();
    void updateNorthLabel(double rawNorthDeg);
    void refreshExpiringTargets();

private:
    void loadPanorama(const QString &path);
    void buildMockTargets();
    void refreshTargetViews();
    void applyBackgroundMode();
    QImage buildFusedPanorama() const;
    QVector<int> visibleTargetIndexes() const;
    double targetDisplayPriority(const TargetRecord &target, qint64 nowMs) const;
    void applySelection();

    TargetListWidget *m_list = nullptr;
    TargetRadarWidget *m_radar = nullptr;
    TargetInfoPanel *m_info = nullptr;
    TargetPreviewPanel *m_preview = nullptr;
    QComboBox *m_mode = nullptr;
    QLabel *m_northLabel = nullptr;
    QPushButton *m_setNorthButton = nullptr;
    QPushButton *m_resetNorthButton = nullptr;
    QTimer *m_targetCleanupTimer = nullptr;
    QVector<TargetRecord> m_targets;
    QStringList m_visibleTargetIds;
    QImage m_rgbPanorama;
    QImage m_bwPanorama;
    QImage m_fusedPanorama;
    bool m_bwNeeds180Align = false;
    int m_selected = 0;
    double m_pendingNorthAzimuth = -1.0;
    int m_maxListTargets = 7;
#ifdef DMX_ADVANCED_DETECTION
    ManualNegativeFeedback m_manualNegativeFeedback;
#endif
};

#endif // TARGETRADARWINDOW_H
