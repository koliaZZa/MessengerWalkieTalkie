#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QTcpServer>
#include <QVector>

#include "authservice.h"

class Connection;
class Worker;

class Server : public QObject
{
    Q_OBJECT

public:
    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    bool start(quint16 port);

private slots:
    void onNewConnection();
    void onConnectionReady(Connection* connection);
    void onPacketReceived(Connection* connection, const QJsonObject& packet);
    void onConnectionClosed(Connection* connection);
    void onAcceptError(QAbstractSocket::SocketError socketError);

private:
    void sendTo(Connection* connection, const QJsonObject& packet, bool reliable = false);
    void broadcastUsers();
    QString usernameFor(Connection* connection) const;
    void unregisterConnection(Connection* connection);

    QTcpServer m_tcpServer;
    QVector<Worker*> m_workers;
    int m_nextWorkerIndex {0};

    mutable QMutex m_mutex;
    QHash<Connection*, QString> m_connectionUsers;
    QHash<QString, Connection*> m_onlineUsers;
    QHash<Connection*, bool> m_allConnections;

    AuthService m_authService;
};
