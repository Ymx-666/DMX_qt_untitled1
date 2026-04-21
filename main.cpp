// 作用：程序入口，开启高 DPI 适配，确保界面在 1080P 和 4K 屏幕上比例一致
#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    // 关键：在创建 app 之前开启缩放支持
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}
