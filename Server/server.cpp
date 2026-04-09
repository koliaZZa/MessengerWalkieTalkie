#include "server.h"

#include "connection.h"
#include "logger.h"
#include "worker.h"
#include "../Shared/authprotocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QMetaObject>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>

namespace {

constexpr char kTypeCheckUser[] = "check_user";
constexpr char kTypeDialogs[] = "dialogs";
constexpr char kTypeHistory[] = "history";
constexpr char kTypeMessage[] = "message";
constexpr char kTypePrivate[] = "private";
constexpr char kTypeRead[] = "read";

const RateLimiter::Rule kRegisterRateLimit{4, 60 * 1000};
const RateLimiter::Rule kLoginRateLimit{6, 60 * 1000};
const RateLimiter::Rule kResumeSessionRateLimit{20, 60 * 1000};
const RateLimiter::Rule kCheckUserRateLimit{20, 10 * 1000};
const RateLimiter::Rule kMessageRateLimit{25, 10 * 1000};
const RateLimiter::Rule kReadReceiptRateLimit{60, 10 * 1000};

}  // namespace

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

    if (!m_storage.init(m_databasePath)) {
        Logger::instance().log(LogLevel::Error, QStringLiteral("Failed to initialize server storage"));
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
        connect(worker, &Worker::connectionReady, this, &Server::onConnectionReady, Qt::BlockingQueuedConnection);
        m_workers.append(worker);
    }

    const bool ok = m_tcpServer.listen(QHostAddress::AnyIPv4, port);
    if (ok) {
        Logger::instance().log(LogLevel::Info,
                               QStringLiteral("Server listening on port %1").arg(port));
    }

    return ok;
}

void Server::setDatabasePath(const QString& databasePath)
{
    if (m_tcpServer.isListening()) {
        return;
    }

    m_databasePath = databasePath.trimmed().isEmpty()
                         ? QStringLiteral("users.db")
                         : databasePath.trimmed();
}

quint16 Server::listeningPort() const
{
    return m_tcpServer.serverPort();
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

    m_connectionUsers.clear();
    m_onlineUsers.clear();

    QThread* ownerThread = QThread::currentThread();
    for (auto worker : workers) {
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
    connect(connection, &Connection::packetReceived, this, &Server::onPacketReceived, Qt::QueuedConnection);
    connect(connection, &Connection::trackedPacketAcknowledged, this, &Server::onTrackedPacketAcknowledged, Qt::QueuedConnection);
    connect(connection, &Connection::closed, this, &Server::onConnectionClosed, Qt::QueuedConnection);
}

void Server::sendAuthError(Connection* connection, const QString& errorMessage)
{
    sendTo(connection,
           {
               {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeAuthError)},
               {AuthProtocol::kFieldMessage, errorMessage}
           });
}

bool Server::canStartAuthenticatedSession(Connection* connection,
                                          const QString& username,
                                          QString& errorMessage) const
{
    const QString currentUsername = m_connectionUsers.value(connection);
    if (!currentUsername.isEmpty() && currentUsername == username) {
        return true;
    }

    if (m_onlineUsers.contains(username) && m_onlineUsers.value(username) != connection) {
        errorMessage = QStringLiteral("User already logged in");
        return false;
    }

    return true;
}

bool Server::attachAuthenticatedSession(Connection* connection,
                                        const QString& username,
                                        QString& errorMessage)
{
    const QString currentUsername = m_connectionUsers.value(connection);
    if (!currentUsername.isEmpty() && currentUsername != username) {
        m_onlineUsers.remove(currentUsername);
        m_connectionUsers.remove(connection);
        m_storage.invalidateSession(currentUsername);
    }

    if (m_onlineUsers.contains(username) && m_onlineUsers.value(username) != connection) {
        errorMessage = QStringLiteral("User already logged in");
        return false;
    }

    m_onlineUsers.insert(username, connection);
    m_connectionUsers.insert(connection, username);
    return true;
}

void Server::finalizeAuthenticatedSession(Connection* connection,
                                          const QString& username,
                                          const AuthProtocol::SessionInfo& sessionInfo)
{
    sendTo(connection, AuthProtocol::makeAuthOkPacket(username, sessionInfo));
    broadcastUsers();
    sendDialogList(connection, username);
    sendPendingPrivateMessages(connection, username);
}

bool Server::handleAuthPacket(Connection* connection, const QString& type, const QJsonObject& packet)
{
    if (type == QString::fromLatin1(AuthProtocol::kTypeRegister)) {
        handleRegisterPacket(connection, packet);
        return true;
    }

    if (type == QString::fromLatin1(AuthProtocol::kTypeLogin)) {
        handleLoginPacket(connection, packet);
        return true;
    }

    if (type == QString::fromLatin1(AuthProtocol::kTypeResumeSession)) {
        handleResumeSessionPacket(connection, packet);
        return true;
    }

    return false;
}

bool Server::handleAuthenticatedPacket(Connection* connection,
                                       const QString& username,
                                       const QString& type,
                                       const QJsonObject& packet)
{
    if (type == QString::fromLatin1(AuthProtocol::kTypeLogout)) {
        handleLogoutPacket(connection, username);
        return true;
    }

    if (type == QString::fromLatin1(kTypeCheckUser)) {
        handleCheckUserPacket(connection, username, packet);
        return true;
    }

    if (type == QString::fromLatin1(kTypeDialogs)) {
        handleDialogsPacket(connection, username);
        return true;
    }

    if (type == QString::fromLatin1(kTypeHistory)) {
        handleHistoryPacket(connection, username, packet);
        return true;
    }

    if (type == QString::fromLatin1(kTypeMessage)) {
        handleBroadcastPacket(connection, username, packet);
        return true;
    }

    if (type == QString::fromLatin1(kTypePrivate)) {
        handlePrivatePacket(connection, username, packet);
        return true;
    }

    if (type == QString::fromLatin1(kTypeRead)) {
        handleReadPacket(connection, username, packet);
        return true;
    }

    return false;
}

void Server::handleRegisterPacket(Connection* connection, const QJsonObject& packet)
{
    const QString username = AuthProtocol::normalizeUsername(packet.value(AuthProtocol::kFieldUsername).toString());
    QString errorMessage;
    if (!allowPeerAction(connection,
                         QStringLiteral("register"),
                         kRegisterRateLimit,
                         QStringLiteral("Too many registration attempts."),
                         errorMessage)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    if (!canStartAuthenticatedSession(connection, username, errorMessage)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    AuthProtocol::SessionInfo sessionInfo;
    if (!m_storage.registerUser(username,
                                packet.value(AuthProtocol::kFieldPassword).toString(),
                                errorMessage,
                                &sessionInfo)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    if (!attachAuthenticatedSession(connection, username, errorMessage)) {
        m_storage.invalidateSession(username);
        sendAuthError(connection, errorMessage);
        return;
    }

    finalizeAuthenticatedSession(connection, username, sessionInfo);
}

void Server::handleLoginPacket(Connection* connection, const QJsonObject& packet)
{
    const QString username = AuthProtocol::normalizeUsername(packet.value(AuthProtocol::kFieldUsername).toString());
    QString errorMessage;
    if (!allowPeerAction(connection,
                         QStringLiteral("login"),
                         kLoginRateLimit,
                         QStringLiteral("Too many login attempts."),
                         errorMessage)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    if (!canStartAuthenticatedSession(connection, username, errorMessage)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    AuthProtocol::SessionInfo sessionInfo;
    if (!m_storage.loginUser(username,
                             packet.value(AuthProtocol::kFieldPassword).toString(),
                             errorMessage,
                             &sessionInfo)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    if (!attachAuthenticatedSession(connection, username, errorMessage)) {
        m_storage.invalidateSession(username);
        sendAuthError(connection, errorMessage);
        return;
    }

    finalizeAuthenticatedSession(connection, username, sessionInfo);
}

void Server::handleResumeSessionPacket(Connection* connection, const QJsonObject& packet)
{
    const QString username = AuthProtocol::normalizeUsername(packet.value(AuthProtocol::kFieldUsername).toString());
    const QString sessionToken = packet.value(AuthProtocol::kFieldSessionToken).toString();
    QString errorMessage;
    if (!allowPeerAction(connection,
                         QStringLiteral("resume_session"),
                         kResumeSessionRateLimit,
                         QStringLiteral("Too many session restore attempts."),
                         errorMessage)) {
        sendTo(connection, AuthProtocol::makeSessionInvalidPacket(errorMessage));
        return;
    }

    if (!canStartAuthenticatedSession(connection, username, errorMessage)) {
        sendAuthError(connection, errorMessage);
        return;
    }

    AuthProtocol::SessionInfo sessionInfo;
    if (!m_storage.resumeSession(username, sessionToken, errorMessage, &sessionInfo)) {
        sendTo(connection, AuthProtocol::makeSessionInvalidPacket(errorMessage));
        return;
    }

    if (!attachAuthenticatedSession(connection, username, errorMessage)) {
        m_storage.invalidateSession(username);
        sendAuthError(connection, errorMessage);
        return;
    }

    finalizeAuthenticatedSession(connection, username, sessionInfo);
}

void Server::handleLogoutPacket(Connection* connection, const QString& username)
{
    m_storage.invalidateSession(username);
    unregisterConnection(connection);
    broadcastUsers();
    QMetaObject::invokeMethod(connection,
                              [connection]() {
                                  connection->closeAndNotify();
                              },
                              Qt::QueuedConnection);
}

void Server::handleCheckUserPacket(Connection* connection, const QString& username, const QJsonObject& packet)
{
    QString errorMessage;
    if (!allowUserAction(connection,
                         username,
                         QStringLiteral("check_user"),
                         kCheckUserRateLimit,
                         QStringLiteral("Too many user lookup requests."),
                         errorMessage)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", errorMessage}
               });
        return;
    }

    const QString lookupUsername = AuthProtocol::normalizeUsername(packet.value("username").toString());
    const bool online = m_onlineUsers.contains(lookupUsername);

    sendTo(connection,
           {
               {"type", "user_check_result"},
               {"username", lookupUsername},
               {"exists", m_storage.userExists(lookupUsername)},
               {"online", online}
           });
}

void Server::handleDialogsPacket(Connection* connection, const QString& username)
{
    sendDialogList(connection, username);
}

void Server::handleHistoryPacket(Connection* connection,
                                 const QString& username,
                                 const QJsonObject& packet)
{
    const QString otherUsername = packet.value("with").toString().trimmed();
    const int limit = qBound(1, packet.value("limit").toInt(100), 200);
    sendHistory(connection, username, otherUsername, limit);
}

void Server::handleBroadcastPacket(Connection* connection,
                                   const QString& username,
                                   const QJsonObject& packet)
{
    const QString messageId = packet.value("id").toString();
    QString errorMessage;
    if (!allowUserAction(connection,
                         username,
                         QStringLiteral("message"),
                         kMessageRateLimit,
                         QStringLiteral("Too many messages."),
                         errorMessage)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", errorMessage},
                   {"id", messageId}
               });
        return;
    }

    const QString text = packet.value("text").toString();
    if (!AuthProtocol::isMessageTextValid(text)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", QStringLiteral("Message must be 1-%1 characters").arg(AuthProtocol::kMaxMessageLength)},
                   {"id", messageId}
               });
        return;
    }

    const qint64 createdAt = QDateTime::currentMSecsSinceEpoch();
    if (!m_storage.storeBroadcastMessage(messageId, username, text, createdAt)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", "Database error"},
                   {"id", messageId}
               });
        return;
    }

    const QList<Connection*> recipients = m_onlineUsers.values();

    for (auto recipient : recipients) {
        sendTo(recipient,
               {
                   {"type", "message"},
                   {"id", messageId},
                   {"from", username},
                   {"text", text},
                   {"created_at", createdAt}
               },
                DeliveryMode::Tracked);
    }
}

void Server::handlePrivatePacket(Connection* connection,
                                 const QString& username,
                                 const QJsonObject& packet)
{
    const QString toUsername = AuthProtocol::normalizeUsername(packet.value("to").toString());
    const QString messageId = packet.value("id").toString();
    const QString text = packet.value("text").toString();
    const qint64 createdAt = QDateTime::currentMSecsSinceEpoch();
    QString errorMessage;

    if (!allowUserAction(connection,
                         username,
                         QStringLiteral("private"),
                         kMessageRateLimit,
                         QStringLiteral("Too many messages."),
                         errorMessage)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", errorMessage},
                   {"id", messageId}
               });
        return;
    }

    if (!AuthProtocol::isPrivateRecipientValid(username, toUsername)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", QStringLiteral("You cannot send private messages to yourself")},
                   {"id", messageId}
               });
        return;
    }

    if (!AuthProtocol::isMessageTextValid(text)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", QStringLiteral("Message must be 1-%1 characters").arg(AuthProtocol::kMaxMessageLength)},
                   {"id", messageId}
               });
        return;
    }

    if (!m_storage.userExists(toUsername)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", QStringLiteral("User not found")},
                   {"id", messageId}
               });
        return;
    }

    if (!m_storage.storePrivateMessage(messageId, username, toUsername, text, createdAt)) {
        sendTo(connection,
               {
                   {"type", "error"},
                   {"message", "Database error"},
                   {"id", messageId}
               });
        return;
    }

    Connection* target = m_onlineUsers.value(toUsername, nullptr);

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
               {"from", username},
               {"to", toUsername},
               {"text", text},
               {"created_at", createdAt}
           },
           DeliveryMode::Tracked);
}

void Server::handleReadPacket(Connection* connection, const QString& username, const QJsonObject& packet)
{
    Connection* target = nullptr;
    const QString toUsername = AuthProtocol::normalizeUsername(packet.value("to").toString());
    const QString messageId = packet.value("id").toString();
    const qint64 readAt = QDateTime::currentMSecsSinceEpoch();
    QString errorMessage;

    if (!allowUserAction(connection,
                         username,
                         QStringLiteral("read"),
                         kReadReceiptRateLimit,
                         QStringLiteral("Too many read receipts."),
                         errorMessage)) {
        return;
    }

    m_storage.markMessageRead(messageId, readAt);
    target = m_onlineUsers.value(toUsername, nullptr);

    if (target) {
        sendTo(target,
               {
                   {"type", "read"},
                   {"id", messageId},
                   {"from", username}
               });
    }
}

bool Server::allowPeerAction(Connection* connection,
                             const QString& actionName,
                             const RateLimiter::Rule& rule,
                             const QString& errorPrefix,
                             QString& errorMessage)
{
    return allowRateLimitedAction(connection,
                                  rateLimitPeerKey(connection),
                                  actionName,
                                  rule,
                                  errorPrefix,
                                  errorMessage);
}

bool Server::allowUserAction(Connection* connection,
                             const QString& username,
                             const QString& actionName,
                             const RateLimiter::Rule& rule,
                             const QString& errorPrefix,
                             QString& errorMessage)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    const QString subjectKey = normalizedUsername.isEmpty()
                                   ? rateLimitPeerKey(connection)
                                   : normalizedUsername;
    return allowRateLimitedAction(connection,
                                  subjectKey,
                                  actionName,
                                  rule,
                                  errorPrefix,
                                  errorMessage);
}

bool Server::allowRateLimitedAction(Connection* connection,
                                    const QString& subjectKey,
                                    const QString& actionName,
                                    const RateLimiter::Rule& rule,
                                    const QString& errorPrefix,
                                    QString& errorMessage)
{
    const QString bucketKey = QStringLiteral("%1:%2")
                                  .arg(actionName)
                                  .arg(subjectKey);
    const RateLimiter::Decision decision = m_rateLimiter.allow(bucketKey, rule);
    if (decision.allowed) {
        return true;
    }

    const qint64 retryAfterSeconds = qMax<qint64>(1, (decision.retryAfterMs + 999) / 1000);
    errorMessage = QStringLiteral("%1 Try again in %2 seconds.")
                       .arg(errorPrefix)
                       .arg(retryAfterSeconds);

    Logger::instance().log(LogLevel::Warning,
                           QStringLiteral("Rate limit hit for %1 on %2 from %3, retry in %4 ms")
                               .arg(actionName)
                               .arg(subjectKey)
                               .arg(rateLimitPeerKey(connection))
                               .arg(QString::number(decision.retryAfterMs)));
    return false;
}

QString Server::rateLimitPeerKey(Connection* connection) const
{
    const QString peer = connection ? connection->peerAddress().trimmed() : QString();
    return peer.isEmpty() ? QStringLiteral("<unknown>") : peer;
}

void Server::onPacketReceived(Connection* connection, const QJsonObject& packet)
{
    const QString type = packet.value(AuthProtocol::kFieldType).toString();
    if (handleAuthPacket(connection, type, packet)) {
        return;
    }

    const QString username = usernameFor(connection);
    if (username.isEmpty()) {
        sendAuthError(connection, QStringLiteral("Please login first"));
        return;
    }

    if (handleAuthenticatedPacket(connection, username, type, packet)) {
        return;
    }

    Logger::instance().log(LogLevel::Warning,
                           QStringLiteral("Unhandled packet type from %1: %2")
                               .arg(connection ? connection->peerAddress() : QStringLiteral("<unknown>"),
                                    type.isEmpty() ? QStringLiteral("<empty>") : type));
}

void Server::onTrackedPacketAcknowledged(Connection*, const QJsonObject& packet)
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
    if (!m_storage.markMessageDelivered(messageId, deliveredAt)) {
        Logger::instance().log(LogLevel::Error,
                               QStringLiteral("Failed to mark message delivered: %1").arg(messageId));
        return;
    }

    Connection* sender = m_onlineUsers.value(fromUsername, nullptr);

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

void Server::sendTo(Connection* connection, const QJsonObject& packet, DeliveryMode deliveryMode)
{
    if (!connection) {
        return;
    }

    if (deliveryMode == DeliveryMode::Tracked) {
        QMetaObject::invokeMethod(connection,
                                  [connection, packet]() {
                                      connection->sendTracked(packet);
                                  },
                                  Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(connection,
                                  [connection, packet]() {
                                      connection->sendPlain(packet);
                                  },
                                  Qt::QueuedConnection);
    }
}

void Server::sendDialogList(Connection* connection, const QString& username)
{
    QJsonArray dialogs;
    const QStringList values = m_storage.loadDialogUsers(username);
    for (auto dialogUser : values) {
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
                                                    ? m_storage.loadBroadcastHistory(limit)
                                                    : m_storage.loadPrivateHistory(username, chatUser, limit);
    for (auto record : records) {
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
    const QList<PendingPrivateMessageRecord> records = m_storage.loadPendingPrivateMessages(username);
    for (auto record : records) {
        sendTo(connection,
               {
                   {"type", "private"},
                   {"id", record.id},
                   {"from", record.from},
                   {"to", record.to},
                   {"text", record.text},
                   {"created_at", record.createdAt}
               },
               DeliveryMode::Tracked);
    }
}

void Server::broadcastUsers()
{
    QJsonArray users;
    QList<Connection*> recipients;

    QStringList usernames = m_onlineUsers.keys();
    std::sort(usernames.begin(), usernames.end());
    for (auto& username : usernames) {
        users.append(username);
    }
    recipients = m_onlineUsers.values();

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
    return m_connectionUsers.value(connection);
}

void Server::unregisterConnection(Connection* connection)
{
    const QString username = m_connectionUsers.take(connection);
    if (!username.isEmpty()) {
        m_onlineUsers.remove(username);
    }
}





