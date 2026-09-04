#include "targetpreviewpanel.h"

#include <QSizePolicy>
#include <QVBoxLayout>

TargetPreviewPanel::TargetPreviewPanel(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    m_title = new QLabel(QStringLiteral("交互图像"), this);
    m_title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 16px;"));
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(320, 320);
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_image->setFixedHeight(360);
    m_image->setStyleSheet(QStringLiteral("background:#101417; border:1px solid #66737a;"));
    m_message = new QLabel(QStringLiteral("点击雷达目标点或方位扇区后显示对应图像。"), this);
    m_message->setWordWrap(true);
    layout->addWidget(m_title);
    layout->addWidget(m_image, 0);
    layout->addWidget(m_message);
}

void TargetPreviewPanel::setImagePath(const QString &path)
{
    m_current = QImage(path);
    updatePixmap();
}

void TargetPreviewPanel::setMessage(const QString &message)
{
    m_message->setText(message);
}

void TargetPreviewPanel::resizeEvent(QResizeEvent *)
{
    updatePixmap();
}

void TargetPreviewPanel::updatePixmap()
{
    if (m_current.isNull()) {
        m_image->setText(QStringLiteral("暂无图像"));
        return;
    }
    m_image->setPixmap(QPixmap::fromImage(m_current).scaled(m_image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
