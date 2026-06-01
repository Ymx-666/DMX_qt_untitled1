#include "mainwindow.h"
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include "appconfig.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    {
        QFontDatabase db;
        const QStringList fams = db.families();
        const QStringList preferred =
            QStringList()
            << QStringLiteral("Microsoft YaHei UI")
            << QStringLiteral("Microsoft YaHei")
            << QStringLiteral("PingFang SC")
            << QStringLiteral("Noto Sans CJK SC")
            << QStringLiteral("Source Han Sans SC")
            << QStringLiteral("SimHei")
            << QStringLiteral("SimSun")
            << QStringLiteral("Arial Unicode MS");
        QString chosen;
        for (const QString &f : preferred) {
            if (fams.contains(f)) {
                chosen = f;
                break;
            }
        }
        if (!chosen.isEmpty()) a.setFont(QFont(chosen, 9));
    }
    {
        QFile qss(QStringLiteral(":/dark_modern.qss"));
        if (qss.open(QFile::ReadOnly | QFile::Text)) {
            a.setStyleSheet(QString::fromUtf8(qss.readAll()));
            qss.close();
        }
    }
    AppConfig::instance();
    MainWindow w;
    w.show();
    return a.exec();
}
