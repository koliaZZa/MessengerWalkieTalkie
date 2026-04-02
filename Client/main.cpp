#include "clientflowcontroller.h"
#include "../Shared/tlsconfiguration.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("MessengerWalkieTalkie"));
    application.setOrganizationName(QStringLiteral("MessengerWalkieTalkie"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MessengerWalkieTalkie client"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption tlsOption(QStringLiteral("tls"),
                                 QStringLiteral("Use TLS for server connections."));
    QCommandLineOption tlsCaOption(QStringLiteral("tls-ca"),
                                   QStringLiteral("PEM CA bundle or server certificate path for TLS verification."),
                                   QStringLiteral("path"));
    QCommandLineOption tlsPeerNameOption(QStringLiteral("tls-peer-name"),
                                         QStringLiteral("Expected TLS peer name. Defaults to the server host."),
                                         QStringLiteral("name"));
    parser.addOption(tlsOption);
    parser.addOption(tlsCaOption);
    parser.addOption(tlsPeerNameOption);
    parser.process(application);

    ClientFlowController controller;
    if (parser.isSet(tlsOption) || parser.isSet(tlsCaOption)) {
        TlsConfiguration::ClientSettings tlsSettings;
        QString tlsError;
        if (!TlsConfiguration::loadClientSettings(parser.value(tlsCaOption),
                                                  parser.value(tlsPeerNameOption),
                                                  &tlsSettings,
                                                  &tlsError)) {
            qCritical().noquote() << tlsError;
            return -1;
        }
        controller.setTlsConfiguration(tlsSettings);
    }

    controller.start();

    return application.exec();
}
