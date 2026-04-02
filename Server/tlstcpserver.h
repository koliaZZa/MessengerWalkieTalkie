#pragma once

#include <QTcpServer>
#include <QSslSocket>

#include "../Shared/tlsconfiguration.h"

class TlsTcpServer : public QTcpServer
{
public:
    explicit TlsTcpServer(QObject* parent = nullptr)
        : QTcpServer(parent)
    {
    }

    void setTlsConfiguration(const TlsConfiguration::ServerSettings& settings)
    {
        m_tlsSettings = settings;
    }

    bool isTlsEnabled() const
    {
        return m_tlsSettings.enabled;
    }

    const TlsConfiguration::ServerSettings& tlsConfiguration() const
    {
        return m_tlsSettings;
    }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        if (!m_tlsSettings.enabled) {
            QTcpServer::incomingConnection(socketDescriptor);
            return;
        }

        auto* socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            socket->deleteLater();
            return;
        }

        socket->setSslConfiguration(TlsConfiguration::makeServerSslConfiguration(m_tlsSettings));
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        addPendingConnection(socket);
    }

private:
    TlsConfiguration::ServerSettings m_tlsSettings;
};
