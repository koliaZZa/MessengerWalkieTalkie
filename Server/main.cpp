#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>

#include "server.h"

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("server"));
    application.setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MessengerWalkieTalkie server"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption({QStringLiteral("p"), QStringLiteral("port")},
                                  QStringLiteral("Listen port."),
                                  QStringLiteral("port"),
                                  QStringLiteral("5555"));
    parser.addOption(portOption);
    parser.process(application);

    const QString portText = parser.value(portOption).trimmed();
    bool portOk = false;
    const uint parsedPort = portText.toUInt(&portOk);
    if (!portOk || parsedPort == 0 || parsedPort > 65535) {
        qCritical().noquote() << QStringLiteral("Invalid port: %1").arg(portText);
        return -1;
    }

    Server server;
    if (!server.start(static_cast<quint16>(parsedPort))) {
        qCritical().noquote() << QStringLiteral("Failed to start server on port %1").arg(parsedPort);
        return -1;
    }

    return application.exec();
}
