#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QVector>

#include "authservice.h"
#include "ratelimiter.h"
#include "tlstcpserver.h"

class Connection;
class Worker;

class Server : public QObject
{
    Q_OBJECT

public:
    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    bool start(quint16 port);
    void setDatabasePath(const QString& databasePath);
    void setTlsConfiguration(const TlsConfiguration::ServerSettings& settings);
    bool isTlsEnabled() const;
    quint16 listeningPort() const;

private slots:
    void onNewConnection();
    void onConnectionReady(Connection* connection);
    void onPacketReceived(Connection* connection, const QJsonObject& packet);
    void onReliablePacketAcked(Connection* connection, const QJsonObject& packet);
    void onConnectionClosed(Connection* connection);
    void onAcceptError(QAbstractSocket::SocketError socketError);

private:
    void stop();
    void sendTo(Connection* connection, const QJsonObject& packet, bool reliable = false);
    void sendAuthError(Connection* connection, const QString& errorMessage);
    bool canStartAuthenticatedSession(Connection* connection, const QString& username, QString& errorMessage) const;
    bool attachAuthenticatedSession(Connection* connection, const QString& username, QString& errorMessage);
    void finalizeAuthenticatedSession(Connection* connection,
                                     const QString& username,
                                     const AuthProtocol::SessionInfo& sessionInfo);
    bool handleAuthPacket(Connection* connection, const QString& type, const QJsonObject& packet);
    bool handleAuthenticatedPacket(Connection* connection,
                                   const QString& username,
                                   const QString& type,
                                   const QJsonObject& packet);
    void handleRegisterPacket(Connection* connection, const QJsonObject& packet);
    void handleLoginPacket(Connection* connection, const QJsonObject& packet);
    void handleResumeSessionPacket(Connection* connection, const QJsonObject& packet);
    void handleLogoutPacket(Connection* connection, const QString& username);
    void handleCheckUserPacket(Connection* connection, const QString& username, const QJsonObject& packet);
    void handleDialogsPacket(Connection* connection, const QString& username);
    void handleHistoryPacket(Connection* connection, const QString& username, const QJsonObject& packet);
    void handleBroadcastPacket(Connection* connection, const QString& username, const QJsonObject& packet);
    void handlePrivatePacket(Connection* connection, const QString& username, const QJsonObject& packet);
    void handleReadPacket(Connection* connection, const QString& username, const QJsonObject& packet);
    bool allowPeerAction(Connection* connection,
                         const QString& actionName,
                         const RateLimiter::Rule& rule,
                         const QString& errorPrefix,
                         QString& errorMessage);
    bool allowUserAction(Connection* connection,
                         const QString& username,
                         const QString& actionName,
                         const RateLimiter::Rule& rule,
                         const QString& errorPrefix,
                         QString& errorMessage);
    bool allowRateLimitedAction(Connection* connection,
                                const QString& subjectKey,
                                const QString& actionName,
                                const RateLimiter::Rule& rule,
                                const QString& errorPrefix,
                                QString& errorMessage);
    QString rateLimitPeerKey(Connection* connection) const;
    void sendDialogList(Connection* connection, const QString& username);
    void sendHistory(Connection* connection, const QString& username, const QString& chatUser, int limit = 100);
    void sendPendingPrivateMessages(Connection* connection, const QString& username);
    void broadcastUsers();
    QString usernameFor(Connection* connection) const;
    void unregisterConnection(Connection* connection);

    TlsTcpServer m_tcpServer;
    QVector<Worker*> m_workers;
    int m_nextWorkerIndex {0};

    mutable QMutex m_mutex;
    QHash<Connection*, QString> m_connectionUsers;
    QHash<QString, Connection*> m_onlineUsers;
    QHash<Connection*, bool> m_allConnections;

    bool m_stopped {false};

    AuthService m_authService;
    RateLimiter m_rateLimiter;
    QString m_databasePath {QStringLiteral("users.db")};
};


