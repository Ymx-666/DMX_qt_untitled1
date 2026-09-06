#include "compacttargetradarpanel.h"

#include "targetradarwidget.h"
#include "../appconfig.h"

#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

static QString compactTargetClass(const QString &className)
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

static QString compactTargetState(const QString &state)
{
    if (state.compare(QStringLiteral("confirmed"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("已确认");
    }
    if (state.compare(QStringLiteral("tracking"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("跟踪中");
    }
    if (state.compare(QStringLiteral("stale"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("临近过期");
    }
    return QStringLiteral("新目标");
}

static QString compactTargetStream(const QString &stream)
{
    const QString normalized = stream.trimmed().toUpper();
    if (normalized.contains(QStringLiteral("RGB"))
        && normalized.contains(QStringLiteral("BW"))) {
        return QStringLiteral("RGB+BW");
    }
    return normalized.isEmpty() ? QStringLiteral("--") : normalized;
}

CompactTargetRadarPanel::CompactTargetRadarPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("compactTargetRadarPanel"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 6, 4);
    layout->setSpacing(8);

    setMinimumHeight(230);

    QWidget *listPane = new QWidget(this);
    listPane->setObjectName(QStringLiteral("compactTargetListPane"));
    QVBoxLayout *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(7, 6, 7, 6);
    listLayout->setSpacing(4);
    QLabel *listTitle = new QLabel(QStringLiteral("识别目标"), listPane);
    listTitle->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;color:#dce9e6;"));
    m_list = new QListWidget(listPane);
    m_list->setObjectName(QStringLiteral("compactTargetList"));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setWordWrap(true);
    m_list->setTextElideMode(Qt::ElideNone);
    m_list->setMinimumWidth(210);
    listLayout->addWidget(listTitle);
    listLayout->addWidget(m_list, 1);

    m_radar = new TargetRadarWidget(this);
    m_radar->setObjectName(QStringLiteral("compactTargetRadar"));
    m_radar->setCompactMode(true);
    m_radar->setMinimumWidth(280);
    m_radar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QWidget *previewPane = new QWidget(this);
    previewPane->setObjectName(QStringLiteral("compactTargetPreviewPane"));
    QVBoxLayout *previewLayout = new QVBoxLayout(previewPane);
    previewLayout->setContentsMargins(7, 6, 7, 6);
    previewLayout->setSpacing(4);
    QLabel *previewTitle = new QLabel(QStringLiteral("目标放大图"), previewPane);
    previewTitle->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;color:#dce9e6;"));
    m_preview = new QLabel(previewPane);
    m_preview->setObjectName(QStringLiteral("compactTargetPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(210, 130);
    m_preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_preview->setStyleSheet(QStringLiteral("background:#050708;border:1px solid #3b4c53;color:#718087;"));
    m_previewDetails = new QLabel(previewPane);
    m_previewDetails->setObjectName(QStringLiteral("compactTargetPreviewDetails"));
    m_previewDetails->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_previewDetails->setWordWrap(true);
    m_previewDetails->setTextFormat(Qt::RichText);
    m_previewDetails->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_previewDetails->setStyleSheet(QStringLiteral("font-size:10px;color:#c7d5d2;"));
    previewLayout->addWidget(previewTitle);
    previewLayout->addWidget(m_preview, 1);
    previewLayout->addWidget(m_previewDetails);

    layout->addWidget(listPane, 3);
    layout->addWidget(m_radar, 5);
    layout->addWidget(previewPane, 3);
    layout->setStretch(0, 3);
    layout->setStretch(1, 5);
    layout->setStretch(2, 3);

    setStyleSheet(QStringLiteral(
        "#compactTargetRadarPanel{background:#090c0f;border:1px solid #314149;}"
        "#compactTargetListPane,#compactTargetPreviewPane{background:#090c0f;}"
        "#compactTargetList{background:#0e1418;border:1px solid #314149;color:#d7e0e3;}"
        "#compactTargetList::item{padding:4px 5px;border-bottom:1px solid #26343a;}"
        "#compactTargetList::item:selected{background:#17342f;color:#f3fffb;border-left:3px solid #55e6c7;}"
        "#compactTargetListPane QLabel,#compactTargetPreviewPane QLabel{background:transparent;}"));

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int index) {
        selectTarget(index);
    });
    connect(m_radar, &TargetRadarWidget::targetClicked, this, [this](int index) {
        selectTarget(index);
    });
    updatePreview();
}

void CompactTargetRadarPanel::setPanorama(const QImage &panorama, bool bwNeeds180Align)
{
    if (panorama.isNull()) return;
    if (!m_panorama.isNull() && panorama.cacheKey() == m_panorama.cacheKey()) return;
    m_panorama = panorama;
    m_radar->setPanoramaBackground(m_panorama, bwNeeds180Align);
}

void CompactTargetRadarPanel::setTargets(const QVector<TargetRecord> &targets)
{
    QString selectedId;
    if (m_selected >= 0 && m_selected < m_targets.size()) {
        selectedId = m_targets.at(m_selected).id;
    }
    m_targets = targets;
    m_radar->setTargets(m_targets);
    rebuildTargetList();
    int index = m_targets.isEmpty() ? -1 : 0;
    if (!selectedId.isEmpty()) {
        for (int i = 0; i < m_targets.size(); ++i) {
            if (m_targets.at(i).id != selectedId) continue;
            index = i;
            break;
        }
    }
    selectTarget(index);
}

void CompactTargetRadarPanel::setScanAngle(double angleDeg)
{
    m_radar->setScanAngle(angleDeg);
}

void CompactTargetRadarPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePreviewPixmap();
}

void CompactTargetRadarPanel::rebuildTargetList()
{
    if (!m_list) return;
    m_list->blockSignals(true);
    m_list->clear();
    if (m_targets.isEmpty()) {
        QListWidgetItem *empty = new QListWidgetItem(QStringLiteral("当前无有效目标"), m_list);
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(QColor(QStringLiteral("#718087")));
    } else {
        for (const TargetRecord &target : m_targets) {
            const double elevation = target.hasElevationAngle
                ? target.elevationAngleDeg
                : 0.0;
            const QString elevationText = target.hasElevationAngle
                ? QStringLiteral("%1°").arg(elevation, 0, 'f', 2)
                : QStringLiteral("--");
            const int confidence = qRound(qBound(0.0, target.confidence, 1.0) * 100.0);
            const QString rowText = QStringLiteral("%1  %2\n方位 %3°  高度 %4  置信 %5%")
                .arg(target.id, compactTargetClass(target.className))
                .arg(target.azimuthDeg, 0, 'f', 1)
                .arg(elevationText)
                .arg(confidence);
            QListWidgetItem *item = new QListWidgetItem(m_list);
            item->setData(Qt::AccessibleTextRole, rowText);
            item->setSizeHint(QSize(0, 50));
            item->setToolTip(QStringLiteral("%1 | %2 | %3")
                .arg(compactTargetStream(target.stream),
                     compactTargetState(target.state),
                     target.imagePath));
            QLabel *row = new QLabel(QStringLiteral(
                "<div><b style='color:#f1f6f5'>%1</b> "
                "<span style='color:#7ee787'>%2</span><br>"
                "<span style='font-size:9px;color:#a9b7bb'>方位 %3°　高度 %4　置信 %5%</span></div>")
                .arg(target.id.toHtmlEscaped(), compactTargetClass(target.className))
                .arg(target.azimuthDeg, 0, 'f', 1)
                .arg(elevationText)
                .arg(confidence),
                m_list);
            row->setObjectName(QStringLiteral("compactTargetListRow"));
            row->setTextFormat(Qt::RichText);
            row->setContentsMargins(5, 2, 3, 2);
            row->setStyleSheet(QStringLiteral("background:transparent;font-size:11px;"));
            row->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_list->setItemWidget(item, row);
        }
    }
    m_list->blockSignals(false);
}

void CompactTargetRadarPanel::selectTarget(int index)
{
    if (index < 0 || index >= m_targets.size()) index = -1;
    m_selected = index;
    if (m_radar) m_radar->setSelectedTarget(index);
    if (m_list && m_list->currentRow() != index) {
        m_list->blockSignals(true);
        m_list->setCurrentRow(index);
        m_list->blockSignals(false);
    }
    updatePreview();
}

void CompactTargetRadarPanel::updatePreview()
{
    if (!m_preview || !m_previewDetails) return;
    const double fov = AppConfig::instance().cameraVerticalFovDeg;
    if (m_selected < 0 || m_selected >= m_targets.size()) {
        m_previewImage = QImage();
        m_previewDetails->setText(QStringLiteral(
            "<b style='color:#f1f6f5'>项目基础信息</b><br>"
            "设备垂直视场角 <span style='color:#8fe5d3'>%1°</span><br>"
            "<span style='color:#718087'>点击雷达红点或左侧目标查看详情</span>")
            .arg(fov, 0, 'f', 2));
        updatePreviewPixmap();
        return;
    }

    const TargetRecord &target = m_targets.at(m_selected);
    QImageReader reader(target.imagePath);
    reader.setAutoTransform(true);
    m_previewImage = reader.read().convertToFormat(QImage::Format_RGB32);
    if (!m_previewImage.isNull() && target.hasRoiBox) {
        const QRect bounds(QPoint(0, 0), m_previewImage.size());
        const QRect box(QPoint(target.roiBoxX1, target.roiBoxY1),
                        QPoint(target.roiBoxX2, target.roiBoxY2));
        const QRect clipped = box.normalized().intersected(bounds);
        if (clipped.isValid()) {
            QPainter painter(&m_previewImage);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(255, 58, 42), 3));
            painter.drawRect(clipped.adjusted(1, 1, -1, -1));
        }
    }

    const QString elevationText = target.hasElevationAngle
        ? QStringLiteral("%1°").arg(target.elevationAngleDeg, 0, 'f', 2)
        : QStringLiteral("--");
    m_previewDetails->setText(QStringLiteral(
        "<b style='color:#f1f6f5'>项目基础信息</b>　%1 %2<br>"
        "设备视场角 <span style='color:#8fe5d3'>%3°</span>　"
        "目标高度角 <span style='color:#ffcf70'>%4</span><br>"
        "方位 <span style='color:#8fe5d3'>%5°</span>　"
        "置信 %6%　来源 %7")
        .arg(target.id.toHtmlEscaped(),
             compactTargetClass(target.className))
        .arg(fov, 0, 'f', 2)
        .arg(elevationText)
        .arg(target.azimuthDeg, 0, 'f', 1)
        .arg(qRound(qBound(0.0, target.confidence, 1.0) * 100.0))
        .arg(compactTargetStream(target.stream).toHtmlEscaped()));
    updatePreviewPixmap();
}

void CompactTargetRadarPanel::updatePreviewPixmap()
{
    if (!m_preview) return;
    if (m_previewImage.isNull()) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(m_selected >= 0
            ? QStringLiteral("目标图像不可用")
            : QStringLiteral("暂无目标"));
        return;
    }
    m_preview->setText(QString());
    const QSize size = m_preview->contentsRect().size();
    if (!size.isValid()) return;
    m_preview->setPixmap(QPixmap::fromImage(m_previewImage).scaled(
        size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
