#pragma once

#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QStringList>

namespace TlsConfiguration {

inline constexpr QSsl::SslProtocol kDefaultProtocol = QSsl::TlsV1_2OrLater;

struct ClientSettings {
    bool enabled = false;
    QList<QSslCertificate> trustedCertificates;
    QString peerVerifyName;
    QSsl::SslProtocol protocol = kDefaultProtocol;

    bool isValid() const
    {
        return !enabled || QSslSocket::supportsSsl();
    }
};

struct ServerSettings {
    bool enabled = false;
    QSslCertificate certificate;
    QSslKey privateKey;
    QSsl::SslProtocol protocol = kDefaultProtocol;

    bool isValid() const
    {
        return !enabled || (!certificate.isNull() && !privateKey.isNull());
    }
};

inline bool ensureTlsSupport(QString* errorMessage = nullptr)
{
    if (QSslSocket::supportsSsl()) {
        return true;
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("TLS is not supported by this Qt build");
    }
    return false;
}

inline QByteArray readPemFile(const QString& path, QString* errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to read TLS file: %1").arg(path);
        }
        return {};
    }

    return file.readAll();
}

inline bool loadCertificatesFromPath(const QString& path,
                                     QList<QSslCertificate>* certificates,
                                     QString* errorMessage = nullptr)
{
    if (!certificates) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("TLS certificate output is not set");
        }
        return false;
    }

    const QByteArray pem = readPemFile(path, errorMessage);
    if (pem.isEmpty()) {
        return false;
    }

    const QList<QSslCertificate> loaded = QSslCertificate::fromData(pem, QSsl::Pem);
    if (loaded.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to parse TLS certificate file: %1").arg(path);
        }
        return false;
    }

    *certificates = loaded;
    return true;
}

inline bool loadPrivateKeyFromPath(const QString& path,
                                   QSslKey* privateKey,
                                   QString* errorMessage = nullptr)
{
    if (!privateKey) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("TLS private key output is not set");
        }
        return false;
    }

    const QByteArray pem = readPemFile(path, errorMessage);
    if (pem.isEmpty()) {
        return false;
    }

    const QSslKey key(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to parse TLS private key file: %1").arg(path);
        }
        return false;
    }

    *privateKey = key;
    return true;
}

inline bool loadServerSettings(const QString& certificatePath,
                               const QString& privateKeyPath,
                               ServerSettings* settings,
                               QString* errorMessage = nullptr)
{
    if (!settings) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("TLS server settings output is not set");
        }
        return false;
    }

    if (!ensureTlsSupport(errorMessage)) {
        return false;
    }

    QList<QSslCertificate> certificates;
    if (!loadCertificatesFromPath(certificatePath, &certificates, errorMessage)) {
        return false;
    }

    QSslKey privateKey;
    if (!loadPrivateKeyFromPath(privateKeyPath, &privateKey, errorMessage)) {
        return false;
    }

    ServerSettings loadedSettings;
    loadedSettings.enabled = true;
    loadedSettings.certificate = certificates.constFirst();
    loadedSettings.privateKey = privateKey;
    *settings = loadedSettings;
    return true;
}

inline bool loadClientSettings(const QString& caCertificatesPath,
                               const QString& peerVerifyName,
                               ClientSettings* settings,
                               QString* errorMessage = nullptr)
{
    if (!settings) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("TLS client settings output is not set");
        }
        return false;
    }

    if (!ensureTlsSupport(errorMessage)) {
        return false;
    }

    ClientSettings loadedSettings;
    loadedSettings.enabled = true;
    loadedSettings.peerVerifyName = peerVerifyName.trimmed();

    if (!caCertificatesPath.trimmed().isEmpty()) {
        if (!loadCertificatesFromPath(caCertificatesPath, &loadedSettings.trustedCertificates, errorMessage)) {
            return false;
        }
    }

    *settings = loadedSettings;
    return true;
}

inline QString peerVerifyName(const ClientSettings& settings, const QString& host)
{
    const QString normalized = settings.peerVerifyName.trimmed();
    return normalized.isEmpty() ? host.trimmed() : normalized;
}

inline QSslConfiguration makeClientSslConfiguration(const ClientSettings& settings)
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setProtocol(settings.protocol);
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);

    if (!settings.trustedCertificates.isEmpty()) {
        QList<QSslCertificate> certificates = configuration.caCertificates();
        for (auto certificate : settings.trustedCertificates) {
            if (!certificates.contains(certificate)) {
                certificates.append(certificate);
            }
        }
        configuration.setCaCertificates(certificates);
    }

    return configuration;
}

inline QSslConfiguration makeServerSslConfiguration(const ServerSettings& settings)
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setProtocol(settings.protocol);
    configuration.setLocalCertificate(settings.certificate);
    configuration.setPrivateKey(settings.privateKey);
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    return configuration;
}

inline QString sslErrorsToString(const QList<QSslError>& errors)
{
    QStringList messages;
    for (auto error : errors) {
        const QString text = error.errorString().trimmed();
        if (!text.isEmpty() && !messages.contains(text)) {
            messages.append(text);
        }
    }

    return messages.isEmpty()
               ? QStringLiteral("TLS handshake failed")
               : QStringLiteral("TLS handshake failed: %1").arg(messages.join(QStringLiteral("; ")));
}

}  // namespace TlsConfiguration
