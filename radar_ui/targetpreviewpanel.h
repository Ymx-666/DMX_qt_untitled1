#ifndef TARGETPREVIEWPANEL_H
#define TARGETPREVIEWPANEL_H

#include <QImage>
#include <QLabel>
#include <QWidget>

class TargetPreviewPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TargetPreviewPanel(QWidget *parent = nullptr);
    void setImagePath(const QString &path);
    void setMessage(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updatePixmap();
    QLabel *m_title = nullptr;
    QLabel *m_image = nullptr;
    QLabel *m_message = nullptr;
    QImage m_current;
};

#endif // TARGETPREVIEWPANEL_H
