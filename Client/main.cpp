#include "chatwindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);

    ChatWindow window;
    window.show();

    return application.exec();
}
