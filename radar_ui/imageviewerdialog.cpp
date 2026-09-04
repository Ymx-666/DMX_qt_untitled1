#include "imageviewerdialog.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

ImageViewerDialog::ImageViewerDialog(QWidget *parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowTitle(QStringLiteral("目标图像查看"));
    resize(960, 720);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);

    QHBoxLayout *tools = new QHBoxLayout();
    QPushButton *zoomOut = new QPushButton(QStringLiteral("缩小"), this);
    QPushButton *zoomIn = new QPushButton(QStringLiteral("放大"), this);
    QPushButton *fit = new QPushButton(QStringLiteral("适合窗口"), this);
    QPushButton *actual = new QPushButton(QStringLiteral("原始大小"), this);
    m_infoLabel = new QLabel(QStringLiteral("--"), this);
    m_infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tools->addWidget(zoomOut);
    tools->addWidget(zoomIn);
    tools->addWidget(fit);
    tools->addWidget(actual);
    tools->addWidget(m_infoLabel, 1);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_imageLabel->setBackgroundRole(QPalette::Base);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_imageLabel);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->viewport()->installEventFilter(this);

    root->addLayout(tools);
    root->addWidget(m_scroll, 1);

    connect(zoomOut, &QPushButton::clicked, this, [this]() { zoom(0.80); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { zoom(1.25); });
    connect(fit, &QPushButton::clicked, this, [this]() { fitToWindow(); });
    connect(actual, &QPushButton::clicked, this, [this]() { setScale(1.0, false); });
}

bool ImageViewerDialog::setImagePath(const QString &path, const QString &title)
{
    m_path = path;
    m_image = QImage(path);
    if (m_image.isNull()) {
        m_infoLabel->setText(QStringLiteral("图像加载失败：%1").arg(path));
        return false;
    }
    if (!title.trimmed().isEmpty()) setWindowTitle(title);
    else setWindowTitle(QFileInfo(path).fileName());
    fitToWindow();
    return true;
}

bool ImageViewerDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scroll->viewport() && event && event->type() == QEvent::Wheel) {
        QWheelEvent *wheel = static_cast<QWheelEvent*>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            zoom(wheel->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ImageViewerDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_fitMode) fitToWindow();
}

void ImageViewerDialog::setScale(double scale, bool fitMode)
{
    m_scale = qBound(0.05, scale, 8.0);
    m_fitMode = fitMode;
    updatePixmap();
}

void ImageViewerDialog::zoom(double factor)
{
    setScale(m_scale * factor, false);
}

void ImageViewerDialog::fitToWindow()
{
    if (m_image.isNull() || !m_scroll) return;
    const QSize view = m_scroll->viewport()->size() - QSize(8, 8);
    if (view.width() <= 0 || view.height() <= 0) return;
    const double sx = (double)view.width() / (double)qMax(1, m_image.width());
    const double sy = (double)view.height() / (double)qMax(1, m_image.height());
    setScale(qMin(sx, sy), true);
}

void ImageViewerDialog::updatePixmap()
{
    if (!m_imageLabel || m_image.isNull()) return;
    const QSize scaledSize(qMax(1, (int)qRound(m_image.width() * m_scale)),
                           qMax(1, (int)qRound(m_image.height() * m_scale)));
    m_imageLabel->setPixmap(QPixmap::fromImage(m_image).scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_imageLabel->resize(scaledSize);
    if (m_infoLabel) {
        m_infoLabel->setText(QStringLiteral("%1  |  %2 x %3  |  %4%")
                             .arg(QFileInfo(m_path).fileName())
                             .arg(m_image.width())
                             .arg(m_image.height())
                             .arg((int)qRound(m_scale * 100.0)));
    }
}
