#include <QCoreApplication>

#include "server.h"

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    Server server;
    if (!server.start(5555)) {
        return -1;
    }

    return application.exec();
}
