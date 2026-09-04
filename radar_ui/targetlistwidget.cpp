#include "targetlistwidget.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

static QString displayTargetState(const QString &state)
{
    if (state.compare(QStringLiteral("confirmed"), Qt::CaseInsensitive) == 0) return QStringLiteral("已确认");
    if (state.compare(QStringLiteral("tracking"), Qt::CaseInsensitive) == 0) return QStringLiteral("跟踪中");
    if (state.compare(QStringLiteral("stale"), Qt::CaseInsensitive) == 0) return QStringLiteral("临近过期");
    return QStringLiteral("新目标");
}

#ifdef DMX_ADVANCED_DETECTION
static QString displayTargetStream(const QString &stream)
{
    const QString upper = stream.trimmed().toUpper();
    if (upper.contains(QStringLiteral("RGB")) && upper.contains(QStringLiteral("BW"))) {
        return QStringLiteral("彩色+黑白");
    }
    if (upper == QStringLiteral("BW")) return QStringLiteral("黑白");
    if (upper == QStringLiteral("RGB")) return QStringLiteral("彩色");
    return stream;
}

static QString displayTargetClass(const QString &className)
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

TargetListWidget::TargetListWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    QLabel *title = new QLabel(QStringLiteral("目标图像列表"), this);
    title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 16px;"));
    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(92, 92));
    m_list->setSpacing(8);
    layout->addWidget(title);
    layout->addWidget(m_list, 1);
    connect(m_list, &QListWidget::currentRowChanged, this, &TargetListWidget::onCurrentRowChanged);
    connect(m_list, &QListWidget::itemActivated, this, &TargetListWidget::onItemActivated);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &TargetListWidget::onItemActivated);
#ifdef DMX_ADVANCED_DETECTION
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setToolTip(QStringLiteral("右键目标可标记误检并保存为训练负样本。"));
    connect(m_list, &QListWidget::customContextMenuRequested,
            this, &TargetListWidget::showTargetContextMenu);
#endif
}

void TargetListWidget::setTargets(const QVector<TargetRecord> &targets, const QVector<int> &sourceIndexes)
{
    m_targets = targets;
    m_sourceIndexes.clear();
    if (sourceIndexes.size() == targets.size()) {
        m_sourceIndexes = sourceIndexes;
    } else {
        for (int i = 0; i < targets.size(); ++i) m_sourceIndexes.push_back(i);
    }

    QSignalBlocker blocker(m_list);
    m_list->clear();
    for (int i = 0; i < m_targets.size(); ++i) {
        const TargetRecord &t = m_targets.at(i);
        const int sourceIndex = sourceIndexForRow(i);
        QListWidgetItem *item = new QListWidgetItem(m_list);
#ifdef DMX_ADVANCED_DETECTION
        item->setSizeHint(QSize(300, 154));
#else
        item->setSizeHint(QSize(300, 118));
#endif
        item->setData(Qt::UserRole, sourceIndex);
        item->setData(Qt::UserRole + 1, t.id);

        QWidget *row = new QWidget(m_list);
        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 8, 8, 8);
        rowLayout->setSpacing(10);

        QLabel *thumb = new QLabel(row);
        thumb->setFixedSize(92, 92);
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setCursor(Qt::PointingHandCursor);
        thumb->setToolTip(QStringLiteral("点击查看目标大图"));
        thumb->setProperty("sourceIndex", sourceIndex);
        thumb->installEventFilter(this);
        thumb->setStyleSheet(QStringLiteral("background:#101417;border:1px solid #4b5962;"));
        QPixmap pm(t.imagePath);
        if (!pm.isNull()) {
            if (t.hasRoiBox && t.roiBoxX2 > t.roiBoxX1 && t.roiBoxY2 > t.roiBoxY1) {
                QPainter painter(&pm);
                QPen pen(QColor(255, 45, 45));
                pen.setWidth(qMax(2, pm.width() / 160));
                painter.setPen(pen);
                painter.drawRect(QRect(QPoint(t.roiBoxX1, t.roiBoxY1),
                                       QPoint(t.roiBoxX2, t.roiBoxY2)).normalized());
                painter.end();
            }
            thumb->setPixmap(pm.scaled(92, 92, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        } else {
            thumb->setText(QStringLiteral("无图"));
        }

        QWidget *texts = new QWidget(row);
        QVBoxLayout *textLayout = new QVBoxLayout(texts);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(2);
        QLabel *id = new QLabel(t.id, texts);
        id->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:#edf5f5;"));
        QLabel *time = new QLabel(t.time.time().toString(QStringLiteral("HH:mm:ss")), texts);
        QLabel *az = new QLabel(QStringLiteral("方位 %1 度").arg(t.azimuthDeg, 0, 'f', 1), texts);
#ifdef DMX_ADVANCED_DETECTION
        QLabel *targetClass = new QLabel(
            QStringLiteral("类别 %1").arg(displayTargetClass(t.className)), texts);
        QLabel *source = new QLabel(QStringLiteral("%1 / 命中%2")
                                   .arg(displayTargetStream(t.stream))
                                   .arg(qMax(1, t.hits)), texts);
        QLabel *score = new QLabel(QStringLiteral("%1 / 评分 %2")
                                   .arg(displayTargetState(t.state))
                                   .arg(t.score, 0, 'f', 0), texts);
#else
        QLabel *score = new QLabel(QStringLiteral("%1 / 命中%2 / 评分 %3")
                                   .arg(displayTargetState(t.state))
                                   .arg(qMax(1, t.hits))
                                   .arg(t.score, 0, 'f', 0), texts);
#endif
        QLabel *box = new QLabel(texts);
        if (t.hasRoiBox) {
            box->setText(QStringLiteral("框 %1,%2-%3,%4")
                         .arg(t.roiBoxX1).arg(t.roiBoxY1)
                         .arg(t.roiBoxX2).arg(t.roiBoxY2));
        } else {
            box->setText(QStringLiteral("框 --"));
        }
        time->setStyleSheet(QStringLiteral("color:#b5c4cc;"));
        az->setStyleSheet(QStringLiteral("color:#91e6d5;"));
#ifdef DMX_ADVANCED_DETECTION
        targetClass->setStyleSheet(QStringLiteral("color:#7ee787;font-weight:600;"));
        source->setStyleSheet(QStringLiteral("color:#d4bfff;"));
#endif
        score->setStyleSheet(QStringLiteral("color:#ffd17c;"));
        box->setStyleSheet(QStringLiteral("color:#ff8f8f;"));
        textLayout->addWidget(id);
        textLayout->addWidget(time);
        textLayout->addWidget(az);
#ifdef DMX_ADVANCED_DETECTION
        textLayout->addWidget(targetClass);
        textLayout->addWidget(source);
#endif
        textLayout->addWidget(score);
        textLayout->addWidget(box);
        textLayout->addStretch();

        rowLayout->addWidget(thumb);
        rowLayout->addWidget(texts, 1);
        m_list->setItemWidget(item, row);
    }
}

void TargetListWidget::setSelectedTarget(int index)
{
    const int row = rowForSourceIndex(index);
    QSignalBlocker blocker(m_list);
    if (row >= 0 && row < m_list->count()) m_list->setCurrentRow(row);
    else m_list->setCurrentRow(-1);
}

bool TargetListWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event && event->type() == QEvent::MouseButtonRelease) {
        const QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() != Qt::LeftButton) {
            return QWidget::eventFilter(watched, event);
        }
        const QVariant v = watched ? watched->property("sourceIndex") : QVariant();
        if (v.isValid()) {
            const int sourceIndex = v.toInt();
            setSelectedTarget(sourceIndex);
            emit targetSelected(sourceIndex);
            emit targetImageRequested(sourceIndex);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TargetListWidget::onCurrentRowChanged(int row)
{
    if (row < 0) return;
    emit targetSelected(sourceIndexForRow(row));
}

void TargetListWidget::onItemActivated(QListWidgetItem *item)
{
    if (!item) return;
    const int sourceIndex = item->data(Qt::UserRole).toInt();
    emit targetImageRequested(sourceIndex);
}

#ifdef DMX_ADVANCED_DETECTION
void TargetListWidget::showTargetContextMenu(const QPoint &position)
{
    QListWidgetItem *item = m_list->itemAt(position);
    if (!item) return;

    const int sourceIndex = item->data(Qt::UserRole).toInt();
    const QString targetId = item->data(Qt::UserRole + 1).toString();
    setSelectedTarget(sourceIndex);
    emit targetSelected(sourceIndex);

    QMenu menu(m_list);
    QAction *markFalsePositive = menu.addAction(QStringLiteral("标记为误检并移除"));
    markFalsePositive->setToolTip(QStringLiteral("保存无标注 ROI 和空 YOLO 标签，然后从当前雷达事件中移除。"));
    if (menu.exec(m_list->viewport()->mapToGlobal(position)) == markFalsePositive) {
        emit falsePositiveRequested(sourceIndex, targetId);
    }
}
#endif

int TargetListWidget::sourceIndexForRow(int row) const
{
    if (row >= 0 && row < m_sourceIndexes.size()) return m_sourceIndexes.at(row);
    return row;
}

int TargetListWidget::rowForSourceIndex(int sourceIndex) const
{
    for (int row = 0; row < m_sourceIndexes.size(); ++row) {
        if (m_sourceIndexes.at(row) == sourceIndex) return row;
    }
    return -1;
}
