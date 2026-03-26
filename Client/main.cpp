#include "clientflowcontroller.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("MessengerWalkieTalkie"));
    application.setOrganizationName(QStringLiteral("MessengerWalkieTalkie"));

    ClientFlowController controller;
    controller.start();

    return application.exec();
}
