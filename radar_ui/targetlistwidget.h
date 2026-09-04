#ifndef TARGETLISTWIDGET_H
#define TARGETLISTWIDGET_H

#include "targetrecord.h"

#include <QListWidget>
#include <QWidget>
#include <QVector>

class TargetListWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TargetListWidget(QWidget *parent = nullptr);
    void setTargets(const QVector<TargetRecord> &targets, const QVector<int> &sourceIndexes = QVector<int>());
    void setSelectedTarget(int index);

signals:
    void targetSelected(int index);
    void targetImageRequested(int index);
#ifdef DMX_ADVANCED_DETECTION
    void falsePositiveRequested(int index, const QString &targetId);
#endif

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onCurrentRowChanged(int row);
    void onItemActivated(QListWidgetItem *item);
#ifdef DMX_ADVANCED_DETECTION
    void showTargetContextMenu(const QPoint &position);
#endif

private:
    int sourceIndexForRow(int row) const;
    int rowForSourceIndex(int sourceIndex) const;

    QListWidget *m_list = nullptr;
    QVector<TargetRecord> m_targets;
    QVector<int> m_sourceIndexes;
};

#endif // TARGETLISTWIDGET_H
