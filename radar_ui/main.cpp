#include "targetradarwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QString panoramaPath;
    if (argc > 1) panoramaPath = QString::fromLocal8Bit(argv[1]);
    TargetRadarWindow w(panoramaPath);
    w.show();
    return app.exec();
}
