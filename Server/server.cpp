#include "server.h"

#include "connection.h"
#include "logger.h"
#include "worker.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QMetaObject>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>

Server::Server(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<Connection*>("Connection*");
}

Server::~Server()
{
    m_tcpServer.close();

    {
        QMutexLocker locker(&m_mutex);
        const auto connections = m_allConnections.keys();
        for (Connection* connection : connections) {
            QMetaObject::invokeMethod(connection, &Connection::closeConnection, Qt::QueuedConnection);
        }
    }

    qDeleteAll(m_workers);
    Logger::instance().stop();
}

bool Server::start(quint16 port)
{
    Logger::instance().start();

    if (!m_authService.init()) {
        Logger::instance().log(LogLevel::Error, QStringLiteral("Failed to initialize auth storage"));
        return false;
    }

    connect(&m_tcpServer, &QTcpServer::newConnection, this, &Server::onNewConnection);
    connect(&m_tcpServer, &QTcpServer::acceptError, this, &Server::onAcceptError);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_tcpServer.close();
    });

    const int workerCount = qMax(2, QThread::idealThreadCount());
    for (int i = 0; i < workerCount; ++i) {
        auto* worker = new Worker();
        connect(worker, &Worker::connectionReady, this, &Server::onConnectionReady, Qt::QueuedConnection);
        m_workers.append(worker);
    }

    const bool ok = m_tcpServer.listen(QHostAddress::AnyIPv4, port);
    if (ok) {
        Logger::instance().log(LogLevel::Info,
                               QStringLiteral("Server listening on port %1").arg(port));
    }

    return ok;
}

void Server::onNewConnection()
{
    while (m_tcpServer.hasPendingConnections()) {
        QTcpSocket* pendingSocket = m_tcpServer.nextPendingConnection();
        if (!pendingSocket) {
            continue;
        }

        pendingSocket->setParent(nullptr);

        Worker* worker = m_workers.at(m_nextWorkerIndex % m_workers.size());
        ++m_nextWorkerIndex;
        pendingSocket->moveToThread(worker->threadHandle());
        worker->attachSocket(pendingSocket);
    }
}

void Server::onConnectionReady(Connection* connection)
{
    {
        QMutexLocker locker(&m_mutex);
        m_allConnections.insert(connection, true);
    }

    connect(connection, &Connection::packetReceived, this, &Server::onPacketReceived, Qt::QueuedConnection);
    connect(connection, &Connection::closed, this, &Server::onConnectionClosed, Qt::QueuedConnection);
}

void Server::onPacketReceived(Connection* connection, const QJsonObject& packet)
{
    const QString type = packet.value("type").toString();
    auto loginConnection = [this, connection](const QString& username, QString& errorMessage) {
        QMutexLocker locker(&m_mutex);

        const QString currentUsername = m_connectionUsers.value(connection);
        if (!currentUsername.isEmpty() && currentUsername != username) {
            m_onlineUsers.remove(currentUsername);
            m_connectionUsers.remove(connection);
        }

        if (m_onlineUsers.contains(username) && m_onlineUsers.value(username) != connection) {
            errorMessage = QStringLiteral("User already logged in");
            return false;
        }

        m_onlineUsers.insert(username, connection);
        m_connectionUsers.insert(connection, username);
        return true;
    };

    if (type == "register") {
        const QString username = packet.value("username").toString().trimmed();
        QString errorMessage;
        const bool ok = m_authService.registerUser(username,
                                                   packet.value("password").toString(),
                                                   errorMessage);

        if (!ok) {
            sendTo(connection, {{"type", "auth_error"}, {"message", errorMessage}});
            return;
        }

        if (!loginConnection(username, errorMessage)) {
            sendTo(connection, {{"type", "auth_error"}, {"message", errorMessage}});
            return;
        }

        sendTo(connection, {{"type", "auth_ok"}, {"username", username}});
        broadcastUsers();
        return;
    }

    if (type == "login") {
        const QString username = packet.value("username").toString().trimmed();
        QString errorMessage;

        if (!m_authService.loginUser(username, packet.value("password").toString(), errorMessage)) {
            sendTo(connection, {{"type", "auth_error"}, {"message", errorMessage}});
            return;
        }

        if (!loginConnection(username, errorMessage)) {
            sendTo(connection, {{"type", "auth_error"}, {"message", errorMessage}});
            return;
        }

        sendTo(connection, {{"type", "auth_ok"}, {"username", username}});
        broadcastUsers();
        return;
    }

    if (type == "check_user") {
        const QString username = packet.value("username").toString().trimmed();
        bool online = false;

        {
            QMutexLocker locker(&m_mutex);
            online = m_onlineUsers.contains(username);
        }

        sendTo(connection,
               {
                   {"type", "user_check_result"},
                   {"username", username},
                   {"exists", m_authService.userExists(username)},
                   {"online", online}
               });
        return;
    }

    const QString from = usernameFor(connection);
    if (from.isEmpty()) {
        sendTo(connection, {{"type", "auth_error"}, {"message", "Please login first"}});
        return;
    }

    if (type == "message") {
        QList<Connection*> recipients;
        {
            QMutexLocker locker(&m_mutex);
            recipients = m_onlineUsers.values();
        }

        for (auto* recipient : recipients) {
            sendTo(recipient,
                   {
                       {"type", "message"},
                       {"id", packet.value("id").toString()},
                       {"from", from},
                       {"text", packet.value("text").toString()}
                   },
                   true);
        }
        return;
    }

    if (type == "private") {
        const QString toUsername = packet.value("to").toString().trimmed();
        Connection* target = nullptr;

        {
            QMutexLocker locker(&m_mutex);
            target = m_onlineUsers.value(toUsername, nullptr);
        }

        if (!target) {
            const QString message = m_authService.userExists(toUsername)
                                        ? QStringLiteral("User is offline")
                                        : QStringLiteral("User not found");
            sendTo(connection,
                   {
                       {"type", "error"},
                       {"message", message},
                       {"id", packet.value("id").toString()}
                   });
            return;
        }

        sendTo(target,
               {
                   {"type", "private"},
                   {"id", packet.value("id").toString()},
                   {"from", from},
                   {"to", toUsername},
                   {"text", packet.value("text").toString()}
               },
               true);

        sendTo(connection,
               {
                   {"type", "delivered"},
                   {"id", packet.value("id").toString()}
               });
        return;
    }

    if (type == "read") {
        Connection* target = nullptr;
        const QString toUsername = packet.value("to").toString().trimmed();

        {
            QMutexLocker locker(&m_mutex);
            target = m_onlineUsers.value(toUsername, nullptr);
        }

        if (target) {
            sendTo(target,
                   {
                       {"type", "read"},
                       {"id", packet.value("id").toString()},
                       {"from", from}
                   });
        }
    }
}

void Server::onConnectionClosed(Connection* connection)
{
    unregisterConnection(connection);
    broadcastUsers();
    connection->deleteLater();
}

void Server::onAcceptError(QAbstractSocket::SocketError)
{
    Logger::instance().log(LogLevel::Error,
                           QStringLiteral("Accept error: %1").arg(m_tcpServer.errorString()));
}

void Server::sendTo(Connection* connection, const QJsonObject& packet, bool reliable)
{
    if (!connection) {
        return;
    }

    if (reliable) {
        QMetaObject::invokeMethod(connection,
                                  [connection, packet]() {
                                      connection->sendReliable(packet);
                                  },
                                  Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(connection,
                                  [connection, packet]() {
                                      connection->sendUnreliable(packet);
                                  },
                                  Qt::QueuedConnection);
    }
}

void Server::broadcastUsers()
{
    QJsonArray users;
    QList<Connection*> recipients;

    {
        QMutexLocker locker(&m_mutex);
        QStringList usernames = m_onlineUsers.keys();
        std::sort(usernames.begin(), usernames.end());
        for (auto& username : usernames) {
            users.append(username);
        }
        recipients = m_onlineUsers.values();
    }

    const QJsonObject packet{
        {"type", "users"},
        {"list", users}
    };

    for (auto* recipient : recipients) {
        sendTo(recipient, packet);
    }
}

QString Server::usernameFor(Connection* connection) const
{
    QMutexLocker locker(&m_mutex);
    return m_connectionUsers.value(connection);
}

void Server::unregisterConnection(Connection* connection)
{
    QMutexLocker locker(&m_mutex);

    m_allConnections.remove(connection);

    const QString username = m_connectionUsers.take(connection);
    if (!username.isEmpty()) {
        m_onlineUsers.remove(username);
    }
}
