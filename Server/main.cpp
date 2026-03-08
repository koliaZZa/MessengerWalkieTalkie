#include <QCoreApplication>
#include "server.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    MessengerServer server;
    if (!server.start(5555)) {
        return -1;
    }

    return a.exec();
}
