#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>

#include "server.h"
#include "../Shared/tlsconfiguration.h"

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
    QCommandLineOption tlsCertificateOption(QStringLiteral("tls-cert"),
                                            QStringLiteral("PEM server certificate path for TLS."),
                                            QStringLiteral("path"));
    QCommandLineOption tlsPrivateKeyOption(QStringLiteral("tls-key"),
                                           QStringLiteral("PEM private key path for TLS."),
                                           QStringLiteral("path"));
    parser.addOption(portOption);
    parser.addOption(tlsCertificateOption);
    parser.addOption(tlsPrivateKeyOption);
    parser.addPositionalArgument(QStringLiteral("port"),
                                 QStringLiteral("Listen port. If specified, overrides the default 5555."));
    parser.process(application);

    QString portText = parser.value(portOption).trimmed();
    const QStringList positionalArguments = parser.positionalArguments();
    if (!positionalArguments.isEmpty() && !parser.isSet(portOption)) {
        portText = positionalArguments.constFirst().trimmed();
    }

    bool portOk = false;
    const uint parsedPort = portText.toUInt(&portOk);
    if (!portOk || parsedPort == 0 || parsedPort > 65535) {
        qCritical().noquote() << QStringLiteral("Invalid port: %1").arg(portText);
        return -1;
    }

    Server server;
    const bool hasTlsCertificate = parser.isSet(tlsCertificateOption);
    const bool hasTlsPrivateKey = parser.isSet(tlsPrivateKeyOption);
    if (hasTlsCertificate != hasTlsPrivateKey) {
        qCritical().noquote() << QStringLiteral("Both --tls-cert and --tls-key must be provided together");
        return -1;
    }

    if (hasTlsCertificate) {
        TlsConfiguration::ServerSettings tlsSettings;
        QString tlsError;
        if (!TlsConfiguration::loadServerSettings(parser.value(tlsCertificateOption),
                                                  parser.value(tlsPrivateKeyOption),
                                                  &tlsSettings,
                                                  &tlsError)) {
            qCritical().noquote() << tlsError;
            return -1;
        }
        server.setTlsConfiguration(tlsSettings);
    }

    if (!server.start(static_cast<quint16>(parsedPort))) {
        qCritical().noquote() << QStringLiteral("Failed to start server on port %1").arg(parsedPort);
        return -1;
    }

    return application.exec();
}
