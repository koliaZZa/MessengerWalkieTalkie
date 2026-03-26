#include "networkclient.h"

#include "protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QUuid>

namespace {

constexpr int kRetryScheduleMs[] = {2000, 4000, 8000, 16000, 32000};
constexpr int kPingIntervalMs = 5000;
constexpr int kPongTimeoutMs = 15000;
constexpr int kReconnectScheduleMs[] = {1000, 2000, 4000, 8000, 16000, 32000};

}

NetworkClient::NetworkClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, &NetworkClient::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &NetworkClient::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &NetworkClient::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &NetworkClient::onSocketError);

    m_pingTimer.setInterval(kPingIntervalMs);
    connect(&m_pingTimer, &QTimer::timeout, this, &NetworkClient::sendPing);

    m_retryTimer.setInterval(1000);
    connect(&m_retryTimer, &QTimer::timeout, this, &NetworkClient::retryPendingMessages);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &NetworkClient::reconnectIfNeeded);
}


void NetworkClient::connectToServer(const QString& host, quint16 port)
{
    m_host = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    m_port = port == 0 ? 5555 : port;
    m_manualDisconnect = false;
    m_reconnectEnabled = true;
    m_reconnectAttempt = 0;
    m_buffer.clear();
    m_pendingMessages.clear();
    m_nextOutgoingSeq = 0;
    m_lastIncomingSeq = 0;
    m_lastPongAtMs = 0;

    m_pingTimer.stop();
    m_retryTimer.stop();
    m_reconnectTimer.stop();
    m_socket.abort();

    startConnectAttempt();
}

void NetworkClient::disconnectFromServer()
{
    m_manualDisconnect = true;
    m_reconnectEnabled = false;
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    m_retryTimer.stop();
    m_pendingMessages.clear();
    m_buffer.clear();

    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        emit statusChanged(QStringLiteral("Disconnected"));
        return;
    }

    m_socket.disconnectFromHost();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
}

bool NetworkClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

QString NetworkClient::host() const
{
    return m_host;
}

quint16 NetworkClient::port() const
{
    return m_port;
}

void NetworkClient::login(const QString& username, const QString& password)
{
    m_username = username.trimmed();

    QJsonObject packet{
        {"type", "login"},
        {"username", m_username},
        {"password", password},
        {"client_mode", QStringLiteral("desktop")},
        {"supports_history", true}
    };

    sendUnreliable(packet);
}

void NetworkClient::registerUser(const QString& username, const QString& password)
{
    m_username = username.trimmed();
    sendUnreliable({
        {"type", "register"},
        {"username", m_username},
        {"password", password}
    });
}

void NetworkClient::checkUserExists(const QString& username)
{
    sendUnreliable({
        {"type", "check_user"},
        {"username", username.trimmed()}
    });
}

void NetworkClient::requestDialogList()
{
    sendUnreliable({{"type", "dialogs"}});
}

void NetworkClient::requestHistory(const QString& chatUser, int limit)
{
    sendUnreliable({
        {"type", "history"},
        {"with", chatUser},
        {"limit", qBound(1, limit, 200)}
    });
}

QString NetworkClient::sendBroadcastMessage(const QString& text)
{
    return sendReliable({
        {"type", "message"},
        {"text", text}
    });
}

QString NetworkClient::sendPrivateMessage(const QString& recipient, const QString& text)
{
    return sendReliable({
        {"type", "private"},
        {"to", recipient},
        {"text", text}
    });
}

void NetworkClient::sendReadReceipt(const QString& recipient, const QString& messageId)
{
    sendUnreliable({
        {"type", "read"},
        {"to", recipient},
        {"id", messageId}
    });
}

void NetworkClient::onConnected()
{
    m_reconnectTimer.stop();
    m_reconnectAttempt = 0;
    m_lastPongAtMs = nowMs();
    m_pingTimer.start();
    m_retryTimer.start();

    emit socketConnected();
    emit statusChanged(QStringLiteral("Connected to %1:%2").arg(m_host).arg(m_port));
}

void NetworkClient::onDisconnected()
{
    m_pingTimer.stop();
    m_retryTimer.stop();

    emit socketDisconnected();

    if (m_manualDisconnect) {
        emit statusChanged(QStringLiteral("Disconnected"));
        m_manualDisconnect = false;
        return;
    }

    scheduleReconnect();
}

void NetworkClient::onReadyRead()
{
    m_buffer.append(m_socket.readAll());

    while (true) {
        QJsonObject packet;
        const Protocol::DecodeStatus status = Protocol::tryDecode(m_buffer, packet);
        if (status == Protocol::DecodeStatus::NeedMoreData) {
            return;
        }

        if (status == Protocol::DecodeStatus::CrcMismatch) {
            emit transportError(QStringLiteral("CRC mismatch"));
            continue;
        }

        if (status == Protocol::DecodeStatus::InvalidJson || status == Protocol::DecodeStatus::InvalidPacket) {
            emit transportError(QStringLiteral("Invalid packet"));
            continue;
        }

        processPacket(packet);
    }
}

void NetworkClient::onSocketError(QAbstractSocket::SocketError)
{
    emit transportError(m_socket.errorString());

    if (!m_manualDisconnect && m_reconnectEnabled
        && m_socket.state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect();
    }
}

void NetworkClient::sendPing()
{
    sendUnreliable({{"type", "ping"}});
}

void NetworkClient::retryPendingMessages()
{
    const qint64 currentMs = nowMs();

    for (auto it = m_pendingMessages.begin(); it != m_pendingMessages.end();) {
        PendingClientMessage& pending = it.value();
        if (currentMs < pending.retryAtMs) {
            ++it;
            continue;
        }

        if (pending.retries >= 5) {
            emit transportError(QStringLiteral("Message dropped after retries: %1").arg(it.key()));
            it = m_pendingMessages.erase(it);
            continue;
        }

        const int delayIndex = qMin(pending.retries + 1, 4);
        pending.retryAtMs = currentMs + kRetryScheduleMs[delayIndex];
        ++pending.retries;
        sendUnreliable(pending.payload);
        ++it;
    }

    if (m_socket.state() == QAbstractSocket::ConnectedState && currentMs - m_lastPongAtMs > kPongTimeoutMs) {
        emit statusChanged(QStringLiteral("Heartbeat timeout, reconnecting"));
        m_socket.abort();
    }
}

void NetworkClient::reconnectIfNeeded()
{
    if (!m_reconnectEnabled || m_manualDisconnect) {
        return;
    }

    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        startConnectAttempt();
    }
}

void NetworkClient::startConnectAttempt()
{
    emit statusChanged(QStringLiteral("Connecting to %1:%2").arg(m_host).arg(m_port));
    m_socket.connectToHost(m_host, m_port);
}

void NetworkClient::scheduleReconnect()
{
    if (!m_reconnectEnabled || m_manualDisconnect || m_reconnectTimer.isActive()) {
        return;
    }

    const int delayMs = nextReconnectDelayMs();
    ++m_reconnectAttempt;
    emit reconnectScheduled(delayMs);
    emit statusChanged(QStringLiteral("Reconnecting to %1:%2 in %3 s")
                           .arg(m_host)
                           .arg(m_port)
                           .arg((delayMs + 999) / 1000));
    m_reconnectTimer.start(delayMs);
}

void NetworkClient::sendUnreliable(const QJsonObject& packet)
{
    Protocol::writePacket(&m_socket, packet);
}

QString NetworkClient::sendReliable(QJsonObject packet)
{
    QString id = packet.value("id").toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        packet["id"] = id;
    }

    packet["seq"] = static_cast<qint64>(++m_nextOutgoingSeq);

    PendingClientMessage pending;
    pending.payload = packet;
    pending.retryAtMs = nowMs() + kRetryScheduleMs[0];
    m_pendingMessages.insert(id, pending);

    sendUnreliable(packet);
    return id;
}

void NetworkClient::processPacket(const QJsonObject& packet)
{
    const QString type = packet.value("type").toString();

    if (type == "ack") {
        m_pendingMessages.remove(packet.value("id").toString());
        return;
    }

    if (type == "pong") {
        m_lastPongAtMs = nowMs();
        return;
    }

    if (type == "ping") {
        sendUnreliable({{"type", "pong"}});
        return;
    }

    const quint32 seq = packet.value("seq").toInt();
    if (seq > 0) {
        const QString id = packet.value("id").toString();
        if (seq <= m_lastIncomingSeq) {
            sendAck(id, seq);
            return;
        }

        m_lastIncomingSeq = seq;
        sendAck(id, seq);
    }

    if (type == "auth_ok") {
        const QString username = packet.value("username").toString();
        if (!username.isEmpty()) {
            m_username = username;
        }
        emit authSucceeded(m_username);
        return;
    }

    if (type == "auth_error") {
        emit authFailed(packet.value("message").toString());
        return;
    }

    if (type == "users") {
        QStringList users;
        const QJsonArray values = packet.value("list").toArray();
        for (const QJsonValue& value : values) {
            users.append(value.toString());
        }
        emit usersUpdated(users);
        return;
    }

    if (type == "dialogs") {
        QStringList dialogs;
        const QJsonArray values = packet.value("list").toArray();
        for (const QJsonValue& value : values) {
            dialogs.append(value.toString());
        }
        emit dialogsReceived(dialogs);
        return;
    }

    if (type == "history") {
        emit historyReceived(packet.value("with").toString(), packet.value("items").toArray());
        return;
    }

    if (type == "user_check_result") {
        emit userLookupFinished(packet.value("username").toString(),
                                packet.value("exists").toBool(),
                                packet.value("online").toBool());
        return;
    }

    if (type == "message") {
        emit publicMessageReceived(packet.value("id").toString(),
                                   packet.value("from").toString(),
                                   packet.value("text").toString(),
                                   packet.value("created_at").toVariant().toLongLong());
        return;
    }

    if (type == "private") {
        emit privateMessageReceived(packet.value("id").toString(),
                                    packet.value("from").toString(),
                                    packet.value("text").toString(),
                                    packet.value("created_at").toVariant().toLongLong());
        return;
    }

    if (type == "queued") {
        emit messageQueued(packet.value("id").toString(),
                           packet.value("to").toString(),
                           packet.value("created_at").toVariant().toLongLong());
        return;
    }

    if (type == "delivered") {
        emit messageDelivered(packet.value("id").toString(),
                              packet.value("created_at").toVariant().toLongLong());
        return;
    }

    if (type == "read") {
        emit messageRead(packet.value("id").toString(), packet.value("from").toString());
        return;
    }

    if (type == "error") {
        emit transportError(packet.value("message").toString());
    }
}

void NetworkClient::sendAck(const QString& id, quint32 seq)
{
    if (id.isEmpty()) {
        return;
    }

    sendUnreliable({
        {"type", "ack"},
        {"id", id},
        {"seq", static_cast<qint64>(seq)}
    });
}

qint64 NetworkClient::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

int NetworkClient::nextReconnectDelayMs() const
{
    const int index = qMin(m_reconnectAttempt, static_cast<int>(std::size(kReconnectScheduleMs)) - 1);
    return kReconnectScheduleMs[index];
}

