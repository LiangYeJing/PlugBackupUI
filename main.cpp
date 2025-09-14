#include "mainwindow.h"

#include <QApplication>

// int main(int argc, char *argv[])
// {
//     QApplication a(argc, argv);
//     MainWindow w;
//     w.show();
//     return a.exec();
// }

int main(int argc, char *argv[]){
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName("PlugBackup");
    QApplication::setOrganizationName("liangyejing");
    QApplication::setOrganizationDomain("lyj.org");

    MainWindow w;
    w.resize(760, 520);
    w.show();
    return app.exec();
}
