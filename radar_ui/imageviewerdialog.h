#ifndef IMAGEVIEWERDIALOG_H
#define IMAGEVIEWERDIALOG_H

#include <QDialog>
#include <QImage>
#include <QString>

class QLabel;
class QScrollArea;

class ImageViewerDialog : public QDialog
{
public:
    explicit ImageViewerDialog(QWidget *parent = nullptr);
    bool setImagePath(const QString &path, const QString &title = QString());

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setScale(double scale, bool fitMode);
    void zoom(double factor);
    void fitToWindow();
    void updatePixmap();

    QLabel *m_imageLabel = nullptr;
    QLabel *m_infoLabel = nullptr;
    QScrollArea *m_scroll = nullptr;
    QImage m_image;
    QString m_path;
    double m_scale = 1.0;
    bool m_fitMode = true;
};

#endif // IMAGEVIEWERDIALOG_H
