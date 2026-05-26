#include "mainwindow.h"
#include <QApplication>
#include <QFont>
#include <QFontDatabase>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    {
        QFontDatabase db;
        const QStringList fams = db.families();
        const QStringList preferred =
            QStringList()
            << QStringLiteral("Microsoft YaHei")
            << QStringLiteral("Microsoft YaHei UI")
            << QStringLiteral("SimHei")
            << QStringLiteral("SimSun")
            << QStringLiteral("NSimSun")
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
    MainWindow w;
    w.show();
    return a.exec();
}
