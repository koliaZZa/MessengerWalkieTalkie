#include "networkclient.h"

#include "protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QUuid>

namespace {

constexpr int kRetryScheduleMs[] = {2000, 4000, 8000, 16000, 32000};
constexpr int kPingIntervalMs = 5000;
constexpr int kPongTimeoutMs = 15000;

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

    m_reconnectTimer.setInterval(3000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &NetworkClient::reconnectIfNeeded);
}

void NetworkClient::connectToServer(const QString& host, quint16 port)
{
    m_host = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    m_port = port;
    m_manualDisconnect = false;

    m_reconnectTimer.stop();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }

    emit statusChanged(QStringLiteral("Connecting to %1:%2").arg(m_host).arg(m_port));
    m_socket.connectToHost(m_host, m_port);
}

void NetworkClient::disconnectFromServer()
{
    m_manualDisconnect = true;
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    m_retryTimer.stop();
    m_pendingMessages.clear();

    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        return;
    }

    m_socket.disconnectFromHost();
}

bool NetworkClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void NetworkClient::login(const QString& username, const QString& password)
{
    m_username = username.trimmed();
    sendUnreliable({
        {"type", "login"},
        {"username", m_username},
        {"password", password}
    });
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
    m_lastPongAtMs = nowMs();
    m_pingTimer.start();
    m_retryTimer.start();

    emit socketConnected();
    emit statusChanged(QStringLiteral("Connected"));
}

void NetworkClient::onDisconnected()
{
    m_pingTimer.stop();
    m_retryTimer.stop();

    emit socketDisconnected();
    emit statusChanged(QStringLiteral("Disconnected"));

    if (!m_manualDisconnect && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }

    m_manualDisconnect = false;
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
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        m_socket.connectToHost(m_host, m_port);
    }
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

    if (type == "user_check_result") {
        emit userLookupFinished(packet.value("username").toString(),
                                packet.value("exists").toBool(),
                                packet.value("online").toBool());
        return;
    }

    if (type == "message") {
        emit publicMessageReceived(packet.value("id").toString(),
                                   packet.value("from").toString(),
                                   packet.value("text").toString());
        return;
    }

    if (type == "private") {
        emit privateMessageReceived(packet.value("id").toString(),
                                    packet.value("from").toString(),
                                    packet.value("text").toString());
        return;
    }

    if (type == "delivered") {
        emit messageDelivered(packet.value("id").toString());
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
