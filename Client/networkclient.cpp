#include "networkclient.h"

#include "protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QUuid>

namespace {

constexpr int kPingIntervalMs = 5000;

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

    connect(&m_packetDispatcher, &NetworkPacketDispatcher::authSucceeded, this, &NetworkClient::authSucceeded);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::authFailed, this, &NetworkClient::authFailed);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::sessionInvalid, this, &NetworkClient::sessionInvalid);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::usersUpdated, this, &NetworkClient::usersUpdated);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::dialogsReceived, this, &NetworkClient::dialogsReceived);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::historyReceived, this, &NetworkClient::historyReceived);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::userLookupFinished, this, &NetworkClient::userLookupFinished);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::publicMessageReceived, this, &NetworkClient::publicMessageReceived);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::privateMessageReceived, this, &NetworkClient::privateMessageReceived);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::messageQueued, this, &NetworkClient::messageQueued);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::messageDelivered, this, &NetworkClient::messageDelivered);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::messageRead, this, &NetworkClient::messageRead);
    connect(&m_packetDispatcher, &NetworkPacketDispatcher::transportError, this, &NetworkClient::transportError);
}

NetworkClient::~NetworkClient()
{
    disconnect(&m_socket, nullptr, this, nullptr);
    m_reconnectTimer.stop();
    m_retryTimer.stop();
    m_pingTimer.stop();

    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
}

void NetworkClient::connectToServer(const QString& host, quint16 port)
{
    m_transportState.startConnection(host, port);

    m_pingTimer.stop();
    m_retryTimer.stop();
    m_reconnectTimer.stop();
    m_socket.abort();

    startConnectAttempt();
}

void NetworkClient::disconnectFromServer()
{
    m_transportState.prepareDisconnect();
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    m_retryTimer.stop();

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
    return m_transportState.host();
}

quint16 NetworkClient::port() const
{
    return m_transportState.port();
}

void NetworkClient::login(const QString& username, const QString& password)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);

    QJsonObject packet{
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeLogin)},
        {AuthProtocol::kFieldUsername, normalizedUsername},
        {AuthProtocol::kFieldPassword, password},
        {"client_mode", QStringLiteral("desktop")},
        {"supports_history", true}
    };

    sendUnreliable(packet);
}

void NetworkClient::registerUser(const QString& username, const QString& password)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    sendUnreliable({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeRegister)},
        {AuthProtocol::kFieldUsername, normalizedUsername},
        {AuthProtocol::kFieldPassword, password}
    });
}

void NetworkClient::resumeSession(const QString& username, const QString& sessionToken)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    sendUnreliable(AuthProtocol::makeResumeSessionPacket(normalizedUsername, sessionToken));
}

void NetworkClient::logout()
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        sendUnreliable(AuthProtocol::makeLogoutPacket());
        m_socket.flush();
        m_socket.waitForBytesWritten(500);
    }
}

void NetworkClient::checkUserExists(const QString& username)
{
    sendUnreliable({
        {"type", "check_user"},
        {"username", AuthProtocol::normalizeUsername(username)}
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
    m_transportState.markConnected(nowMs());
    m_pingTimer.start();
    m_retryTimer.start();

    emit socketConnected();
    emit statusChanged(QStringLiteral("Connected to %1:%2")
                           .arg(m_transportState.host())
                           .arg(m_transportState.port()));
}

void NetworkClient::onDisconnected()
{
    m_pingTimer.stop();
    m_retryTimer.stop();

    emit socketDisconnected();

    if (m_transportState.consumeManualDisconnect()) {
        emit statusChanged(QStringLiteral("Disconnected"));
        return;
    }

    scheduleReconnect();
}

void NetworkClient::onReadyRead()
{
    QByteArray& buffer = m_transportState.incomingBuffer();
    buffer.append(m_socket.readAll());

    while (true) {
        QJsonObject packet;
        const Protocol::DecodeStatus status = Protocol::tryDecode(buffer, packet);
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

    if (m_transportState.canReconnect() && m_socket.state() == QAbstractSocket::UnconnectedState) {
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
    const RetryActions actions = m_transportState.collectRetryActions(currentMs);

    for (auto droppedId : actions.droppedMessageIds) {
        emit transportError(QStringLiteral("Message dropped after retries: %1").arg(droppedId));
    }

    for (auto packet : actions.resendPackets) {
        sendUnreliable(packet);
    }

    if (m_socket.state() == QAbstractSocket::ConnectedState && m_transportState.isHeartbeatExpired(currentMs)) {
        emit statusChanged(QStringLiteral("Heartbeat timeout, reconnecting"));
        m_socket.abort();
    }
}

void NetworkClient::reconnectIfNeeded()
{
    if (!m_transportState.canReconnect()) {
        return;
    }

    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        startConnectAttempt();
    }
}

void NetworkClient::startConnectAttempt()
{
    emit statusChanged(QStringLiteral("Connecting to %1:%2")
                           .arg(m_transportState.host())
                           .arg(m_transportState.port()));
    m_socket.connectToHost(m_transportState.host(), m_transportState.port());
}

void NetworkClient::scheduleReconnect()
{
    if (!m_transportState.canReconnect() || m_reconnectTimer.isActive()) {
        return;
    }

    const int delayMs = m_transportState.scheduleReconnect();
    if (delayMs <= 0) {
        return;
    }
    emit reconnectScheduled(delayMs);
    emit statusChanged(QStringLiteral("Reconnecting to %1:%2 in %3 s")
                           .arg(m_transportState.host())
                           .arg(m_transportState.port())
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

    packet["seq"] = static_cast<qint64>(m_transportState.nextOutgoingSequence());
    m_transportState.trackReliableMessage(id, packet, nowMs());

    sendUnreliable(packet);
    return id;
}

void NetworkClient::processPacket(const QJsonObject& packet)
{
    const QString type = packet.value("type").toString();

    if (type == "ack") {
        m_transportState.acknowledgeReliableMessage(packet.value("id").toString());
        return;
    }

    if (type == "pong") {
        m_transportState.markPongReceived(nowMs());
        return;
    }

    if (type == "ping") {
        sendUnreliable({{"type", "pong"}});
        return;
    }

    const quint32 seq = packet.value("seq").toInt();
    if (seq > 0) {
        const QString id = packet.value("id").toString();
        if (m_transportState.isDuplicateIncomingSequence(seq)) {
            sendAck(id, seq);
            return;
        }

        m_transportState.markIncomingSequence(seq);
        sendAck(id, seq);
    }

    QString errorMessage;
    if (m_packetDispatcher.dispatch(packet, &errorMessage) == NetworkPacketDispatcher::DispatchResult::InvalidPacket) {
        emit transportError(errorMessage);
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
