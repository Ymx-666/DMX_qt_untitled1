#include "targetinfopanel.h"

#include "../appconfig.h"
#include <QVBoxLayout>

static QLabel *makeValue(QWidget *parent)
{
    QLabel *l = new QLabel(QStringLiteral("--"), parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

static QString displayStream(const QString &stream)
{
    const QString upper = stream.trimmed().toUpper();
    if (upper.contains(QStringLiteral("RGB")) && upper.contains(QStringLiteral("BW"))) {
        return QStringLiteral("彩色 + 黑白");
    }
    if (stream.compare(QStringLiteral("BW"), Qt::CaseInsensitive) == 0) return QStringLiteral("黑白");
    if (stream.compare(QStringLiteral("RGB"), Qt::CaseInsensitive) == 0) return QStringLiteral("彩色");
    return stream;
}

static QString displayState(const QString &state)
{
    if (state.compare(QStringLiteral("new / unconfirmed"), Qt::CaseInsensitive) == 0) return QStringLiteral("新目标/未确认");
    if (state.compare(QStringLiteral("new"), Qt::CaseInsensitive) == 0) return QStringLiteral("新目标/未确认");
    if (state.compare(QStringLiteral("confirmed"), Qt::CaseInsensitive) == 0) return QStringLiteral("已确认");
    if (state.compare(QStringLiteral("tracking"), Qt::CaseInsensitive) == 0) return QStringLiteral("跟踪中");
    if (state.compare(QStringLiteral("stale"), Qt::CaseInsensitive) == 0) return QStringLiteral("临近过期");
    return state;
}

#ifdef DMX_ADVANCED_DETECTION
static QString displayClass(const QString &className)
{
    const QString normalized = className.trimmed().toLower();
    if (normalized == QStringLiteral("drone")) return QStringLiteral("无人机");
    if (normalized == QStringLiteral("bird")) return QStringLiteral("飞鸟");
    if (normalized == QStringLiteral("civilian_airliners")
        || normalized == QStringLiteral("civilian_airliner")
        || normalized == QStringLiteral("airliner")) {
        return QStringLiteral("民航客机");
    }
    return QStringLiteral("未知目标");
}
#endif

TargetInfoPanel::TargetInfoPanel(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    QLabel *title = new QLabel(QStringLiteral("选中目标信息"), this);
    title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 16px;"));
    QFormLayout *form = new QFormLayout();
    m_id = makeValue(this);
    m_time = makeValue(this);
    m_azimuth = makeValue(this);
    m_pano = makeValue(this);
    m_verticalFov = makeValue(this);
    m_verticalFov->setObjectName(QStringLiteral("targetVerticalFovValue"));
    m_elevation = makeValue(this);
    m_elevation->setObjectName(QStringLiteral("targetElevationValue"));
    m_frame = makeValue(this);
    m_stream = makeValue(this);
#ifdef DMX_ADVANCED_DETECTION
    m_class = makeValue(this);
#endif
    m_confidence = makeValue(this);
    m_state = makeValue(this);
    form->addRow(QStringLiteral("编号"), m_id);
    form->addRow(QStringLiteral("时间"), m_time);
    form->addRow(QStringLiteral("方位角"), m_azimuth);
    form->addRow(QStringLiteral("全景坐标"), m_pano);
    form->addRow(QStringLiteral("设备垂直视场角"), m_verticalFov);
    form->addRow(QStringLiteral("目标高度角"), m_elevation);
    form->addRow(QStringLiteral("帧坐标"), m_frame);
    form->addRow(QStringLiteral("视频源"), m_stream);
#ifdef DMX_ADVANCED_DETECTION
    form->addRow(QStringLiteral("目标类别"), m_class);
#endif
    form->addRow(QStringLiteral("置信度"), m_confidence);
    form->addRow(QStringLiteral("状态"), m_state);
    outer->addWidget(title);
    outer->addLayout(form);
}

void TargetInfoPanel::setTarget(const TargetRecord &t)
{
    m_id->setText(t.id);
    m_time->setText(t.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    m_azimuth->setText(QStringLiteral("%1 度").arg(t.azimuthDeg, 0, 'f', 1));
    m_pano->setText(QStringLiteral("%1 / %2").arg(t.panoX).arg(t.panoY));
    m_verticalFov->setText(QStringLiteral("%1 度")
        .arg(AppConfig::instance().cameraVerticalFovDeg, 0, 'f', 2));
    m_elevation->setText(t.hasElevationAngle
        ? QStringLiteral("%1 度").arg(t.elevationAngleDeg, 0, 'f', 2)
        : QStringLiteral("--"));
    m_frame->setText(QStringLiteral("%1 / %2").arg(t.frameX).arg(t.frameY));
    m_stream->setText(displayStream(t.stream));
#ifdef DMX_ADVANCED_DETECTION
    m_class->setText(displayClass(t.className));
#endif
    m_confidence->setText(QStringLiteral("%1").arg(t.confidence, 0, 'f', 2));
    m_state->setText(displayState(t.state));
}

void TargetInfoPanel::clearTarget()
{
    QList<QLabel*> labels = {m_id, m_time, m_azimuth, m_pano, m_frame, m_stream, m_confidence, m_state};
    labels.push_back(m_verticalFov);
    labels.push_back(m_elevation);
#ifdef DMX_ADVANCED_DETECTION
    labels.push_back(m_class);
#endif
    for (QLabel *l : labels) if (l) l->setText(QStringLiteral("--"));
}
