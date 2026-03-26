#include "server.h"

#include "connection.h"
#include "logger.h"
#include "worker.h"

#include <QCoreApplication>
#include <QDateTime>
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
    stop();
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
        stop();
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

void Server::stop()
{
    if (m_stopped) {
        return;
    }

    m_stopped = true;
    m_tcpServer.close();
    disconnect(&m_tcpServer, nullptr, this, nullptr);

    const auto workers = m_workers;
    m_workers.clear();

    {
        QMutexLocker locker(&m_mutex);
        m_connectionUsers.clear();
        m_onlineUsers.clear();
        m_allConnections.clear();
    }

    QThread* ownerThread = QThread::currentThread();
    for (Worker* worker : workers) {
        worker->shutdown(ownerThread);
        delete worker;
    }

    Logger::instance().stop();
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
    connect(connection, &Connection::reliablePacketAcked, this, &Server::onReliablePacketAcked, Qt::QueuedConnection);
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
        sendDialogList(connection, username);
        sendPendingPrivateMessages(connection, username);
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
        sendDialogList(connection, username);
        sendPendingPrivateMessages(connection, username);
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

    if (type == "dialogs") {
        sendDialogList(connection, from);
        return;
    }

    if (type == "history") {
        const QString otherUsername = packet.value("with").toString().trimmed();
        const int limit = qBound(1, packet.value("limit").toInt(100), 200);
        sendHistory(connection, from, otherUsername, limit);
        return;
    }

    if (type == "message") {
        const QString messageId = packet.value("id").toString();
        const qint64 createdAt = QDateTime::currentMSecsSinceEpoch();
        if (!m_authService.storeBroadcastMessage(messageId, from, packet.value("text").toString(), createdAt)) {
            sendTo(connection,
                   {
                       {"type", "error"},
                       {"message", "Database error"},
                       {"id", messageId}
                   });
            return;
        }

        QList<Connection*> recipients;
        {
            QMutexLocker locker(&m_mutex);
            recipients = m_onlineUsers.values();
        }

        for (auto* recipient : recipients) {
            sendTo(recipient,
                   {
                       {"type", "message"},
                       {"id", messageId},
                       {"from", from},
                       {"text", packet.value("text").toString()},
                       {"created_at", createdAt}
                   },
                   true);
        }
        return;
    }

    if (type == "private") {
        const QString toUsername = packet.value("to").toString().trimmed();
        const QString messageId = packet.value("id").toString();
        const QString text = packet.value("text").toString();
        const qint64 createdAt = QDateTime::currentMSecsSinceEpoch();

        if (!m_authService.userExists(toUsername)) {
            sendTo(connection,
                   {
                       {"type", "error"},
                       {"message", QStringLiteral("User not found")},
                       {"id", messageId}
                   });
            return;
        }

        if (!m_authService.storePrivateMessage(messageId, from, toUsername, text, createdAt)) {
            sendTo(connection,
                   {
                       {"type", "error"},
                       {"message", "Database error"},
                       {"id", messageId}
                   });
            return;
        }

        Connection* target = nullptr;

        {
            QMutexLocker locker(&m_mutex);
            target = m_onlineUsers.value(toUsername, nullptr);
        }

        if (!target) {
            sendTo(connection,
                   {
                       {"type", "queued"},
                       {"id", messageId},
                       {"to", toUsername},
                       {"created_at", createdAt}
                   });
            return;
        }

        sendTo(target,
               {
                   {"type", "private"},
                   {"id", messageId},
                   {"from", from},
                   {"to", toUsername},
                   {"text", text},
                   {"created_at", createdAt}
               },
               true);
        return;
    }

    if (type == "read") {
        Connection* target = nullptr;
        const QString toUsername = packet.value("to").toString().trimmed();
        const QString messageId = packet.value("id").toString();
        const qint64 readAt = QDateTime::currentMSecsSinceEpoch();

        m_authService.markMessageRead(messageId, readAt);

        {
            QMutexLocker locker(&m_mutex);
            target = m_onlineUsers.value(toUsername, nullptr);
        }

        if (target) {
            sendTo(target,
                   {
                       {"type", "read"},
                       {"id", messageId},
                       {"from", from}
                   });
        }
    }
}

void Server::onReliablePacketAcked(Connection*, const QJsonObject& packet)
{
    if (packet.value("type").toString() != QStringLiteral("private")) {
        return;
    }

    const QString messageId = packet.value("id").toString();
    const QString fromUsername = packet.value("from").toString();
    const QString toUsername = packet.value("to").toString();
    if (messageId.isEmpty() || fromUsername.isEmpty() || toUsername.isEmpty()) {
        return;
    }

    const qint64 deliveredAt = QDateTime::currentMSecsSinceEpoch();
    if (!m_authService.markMessageDelivered(messageId, deliveredAt)) {
        Logger::instance().log(LogLevel::Error,
                               QStringLiteral("Failed to mark message delivered: %1").arg(messageId));
        return;
    }

    Connection* sender = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        sender = m_onlineUsers.value(fromUsername, nullptr);
    }

    if (sender) {
        sendTo(sender,
               {
                   {"type", "delivered"},
                   {"id", messageId},
                   {"to", toUsername},
                   {"created_at", packet.value("created_at").toVariant().toLongLong()}
               });
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

void Server::sendDialogList(Connection* connection, const QString& username)
{
    QJsonArray dialogs;
    const QStringList values = m_authService.loadDialogUsers(username);
    for (const QString& dialogUser : values) {
        dialogs.append(dialogUser);
    }

    sendTo(connection,
           {
               {"type", "dialogs"},
               {"list", dialogs}
           });
}

void Server::sendHistory(Connection* connection, const QString& username, const QString& chatUser, int limit)
{
    if (chatUser.isEmpty()) {
        sendTo(connection,
               {
                   {"type", "history"},
                   {"with", chatUser},
                   {"items", QJsonArray()}
               });
        return;
    }

    QJsonArray items;
    const QList<HistoryMessageRecord> records = chatUser == QStringLiteral("Broadcast")
                                                    ? m_authService.loadBroadcastHistory(limit)
                                                    : m_authService.loadPrivateHistory(username, chatUser, limit);
    for (const HistoryMessageRecord& record : records) {
        items.append(QJsonObject{
            {"id", record.id},
            {"from", record.from},
            {"to", record.to},
            {"text", record.text},
            {"status", record.status},
            {"created_at", record.createdAt}
        });
    }

    sendTo(connection,
           {
               {"type", "history"},
               {"with", chatUser},
               {"items", items}
           });
}

void Server::sendPendingPrivateMessages(Connection* connection, const QString& username)
{
    const QList<PendingPrivateMessageRecord> records = m_authService.loadPendingPrivateMessages(username);
    for (const PendingPrivateMessageRecord& record : records) {
        sendTo(connection,
               {
                   {"type", "private"},
                   {"id", record.id},
                   {"from", record.from},
                   {"to", record.to},
                   {"text", record.text},
                   {"created_at", record.createdAt}
               },
               true);
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

