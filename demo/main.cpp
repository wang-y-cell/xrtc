#include "widget.h"

#include <QApplication>
#include <iostream>
#include <windows.h>

int main(int argc, char *argv[])
{
    //终端设置
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::cout << "start" << std::endl;
    QApplication a(argc, argv);
    Widget w;
    w.show();
    return QCoreApplication::exec();
}
