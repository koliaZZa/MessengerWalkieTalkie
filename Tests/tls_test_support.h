#pragma once

#include <QString>

#include "../Shared/tlsconfiguration.h"

namespace TestSupport {

inline QString tlsFixtureDirectory()
{
    return QStringLiteral(QT_TESTCASE_SOURCEDIR) + QStringLiteral("/data/tls");
}

inline QString tlsCertificatePath()
{
    return tlsFixtureDirectory() + QStringLiteral("/localhost-cert.pem");
}

inline QString tlsPrivateKeyPath()
{
    return tlsFixtureDirectory() + QStringLiteral("/localhost-key.pem");
}

inline bool loadTestServerTlsSettings(TlsConfiguration::ServerSettings* settings,
                                      QString* errorMessage = nullptr)
{
    return TlsConfiguration::loadServerSettings(tlsCertificatePath(),
                                                tlsPrivateKeyPath(),
                                                settings,
                                                errorMessage);
}

inline bool loadTestClientTlsSettings(TlsConfiguration::ClientSettings* settings,
                                      QString* errorMessage = nullptr)
{
    return TlsConfiguration::loadClientSettings(tlsCertificatePath(),
                                                QString(),
                                                settings,
                                                errorMessage);
}

}  // namespace TestSupport
