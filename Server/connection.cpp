#include "connection.h"

#include "logger.h"
#include "protocol.h"
#include "../Shared/tlsconfiguration.h"

#include <QDateTime>
#include <QSslSocket>
#include <QUuid>

namespace {

constexpr int kRetryScheduleMs[] = {2000, 4000, 8000, 16000, 32000};
constexpr int kPingIntervalMs = 5000;
constexpr int kPongTimeoutMs = 15000;

}

Connection::Connection(QTcpSocket* socket, QObject* parent)
    : QObject(parent)
    , m_socket(socket)
{
    qRegisterMetaType<Connection*>("Connection*");

    m_retryTimer.setInterval(1000);
    connect(&m_retryTimer, &QTimer::timeout, this, &Connection::checkPendingMessages);

    m_pingTimer.setInterval(kPingIntervalMs);
    connect(&m_pingTimer, &QTimer::timeout, this, &Connection::sendPing);
}

Connection::~Connection()
{
    closeConnection();
}

QString Connection::peerAddress() const
{
    return m_socket ? m_socket->peerAddress().toString() : QStringLiteral("<unknown>");
}

void Connection::start()
{
    if (m_socket) {
        m_socket->setParent(this);
    }

    connect(m_socket, &QTcpSocket::readyRead, this, &Connection::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &Connection::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &Connection::onSocketError);

    if (auto* sslSocket = qobject_cast<QSslSocket*>(m_socket)) {
        connect(sslSocket, &QSslSocket::encrypted, this, &Connection::onEncrypted);
        connect(sslSocket, &QSslSocket::sslErrors, this, &Connection::onSslErrors);

        Logger::instance().log(LogLevel::Info,
                               QStringLiteral("Starting TLS handshake with %1").arg(peerAddress()));
        sslSocket->startServerEncryption();
        if (sslSocket->isEncrypted()) {
            finishStartup();
        }
        return;
    }

    finishStartup();
}

void Connection::sendUnreliable(const QJsonObject& packet)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    if (auto* sslSocket = qobject_cast<QSslSocket*>(m_socket); sslSocket && !sslSocket->isEncrypted()) {
        return;
    }

    Protocol::writePacket(m_socket, packet);
}

void Connection::sendReliable(QJsonObject packet)
{
    QString messageId = packet.value("id").toString();
    if (messageId.isEmpty()) {
        messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        packet["id"] = messageId;
    }

    packet["seq"] = static_cast<qint64>(++m_nextOutgoingSeq);

    PendingMessage pending;
    pending.payload = packet;
    pending.retryAtMs = nowMs() + kRetryScheduleMs[0];
    m_pendingMessages.insert(messageId, pending);

    sendUnreliable(packet);
}

void Connection::closeConnection()
{
    m_retryTimer.stop();
    m_pingTimer.stop();

    if (!m_socket) {
        return;
    }

    disconnect(m_socket, nullptr, this, nullptr);

    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->abort();
        }
    }

    m_socket->deleteLater();
    m_socket = nullptr;
}

void Connection::onReadyRead()
{
    if (!m_socket) {
        return;
    }

    if (auto* sslSocket = qobject_cast<QSslSocket*>(m_socket); sslSocket && !sslSocket->isEncrypted()) {
        return;
    }

    m_buffer.append(m_socket->readAll());

    while (true) {
        QJsonObject packet;
        const Protocol::DecodeStatus status = Protocol::tryDecode(m_buffer, packet);
        if (status == Protocol::DecodeStatus::NeedMoreData) {
            return;
        }

        if (status == Protocol::DecodeStatus::CrcMismatch) {
            Logger::instance().log(LogLevel::Error,
                                   QStringLiteral("CRC mismatch from %1").arg(peerAddress()));
            continue;
        }

        if (status == Protocol::DecodeStatus::InvalidJson || status == Protocol::DecodeStatus::InvalidPacket) {
            Logger::instance().log(LogLevel::Error,
                                   QStringLiteral("Invalid packet from %1").arg(peerAddress()));
            continue;
        }

        processIncomingPacket(packet);
    }
}

void Connection::onEncrypted()
{
    finishStartup();
}

void Connection::onSocketDisconnected()
{
    Logger::instance().log(LogLevel::Warning,
                           QStringLiteral("%1 disconnected: %2")
                               .arg(qobject_cast<QSslSocket*>(m_socket) ? QStringLiteral("TLS client")
                                                                        : QStringLiteral("Client"))
                               .arg(peerAddress()));
    emit closed(this);
}

void Connection::onSocketError(QAbstractSocket::SocketError)
{
    if (!m_socket) {
        return;
    }

    Logger::instance().log(LogLevel::Warning,
                           QStringLiteral("Socket error from %1: %2")
                               .arg(peerAddress(), m_socket->errorString()));
}

void Connection::onSslErrors(const QList<QSslError>& errors)
{
    Logger::instance().log(LogLevel::Warning,
                           QStringLiteral("TLS error from %1: %2")
                               .arg(peerAddress(), TlsConfiguration::sslErrorsToString(errors)));
    closeConnection();
    emit closed(this);
}

void Connection::checkPendingMessages()
{
    const qint64 currentMs = nowMs();

    for (auto it = m_pendingMessages.begin(); it != m_pendingMessages.end();) {
        PendingMessage& pending = it.value();
        if (currentMs < pending.retryAtMs) {
            ++it;
            continue;
        }

        if (pending.retries >= 5) {
            Logger::instance().log(LogLevel::Warning,
                                   QStringLiteral("Drop message after retries: %1").arg(it.key()));
            it = m_pendingMessages.erase(it);
            continue;
        }

        const int delayIndex = qMin(pending.retries + 1, 4);
        pending.retryAtMs = currentMs + kRetryScheduleMs[delayIndex];
        ++pending.retries;

        Logger::instance().log(LogLevel::Warning,
                               QStringLiteral("Retry message %1 attempt %2")
                                   .arg(it.key())
                                   .arg(pending.retries));
        sendUnreliable(pending.payload);
        ++it;
    }

    if (currentMs - m_lastPongAtMs > kPongTimeoutMs) {
        Logger::instance().log(LogLevel::Warning,
                               QStringLiteral("Heartbeat timeout from %1").arg(peerAddress()));
        closeConnection();
        emit closed(this);
    }
}

void Connection::sendPing()
{
    sendUnreliable({{"type", "ping"}});
}

void Connection::processIncomingPacket(const QJsonObject& packet)
{
    const QString type = packet.value("type").toString();

    if (type == "ack") {
        const QString messageId = packet.value("id").toString();
        const auto it = m_pendingMessages.find(messageId);
        if (it != m_pendingMessages.end()) {
            emit reliablePacketAcked(this, it.value().payload);
            m_pendingMessages.erase(it);
        }
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

    emit packetReceived(this, packet);
}

void Connection::sendAck(const QString& messageId, quint32 seq)
{
    if (messageId.isEmpty()) {
        return;
    }

    sendUnreliable({
        {"type", "ack"},
        {"id", messageId},
        {"seq", static_cast<qint64>(seq)}
    });
}

qint64 Connection::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

void Connection::finishStartup()
{
    if (m_transportReady) {
        return;
    }

    m_transportReady = true;
    m_lastPongAtMs = nowMs();
    m_retryTimer.start();
    m_pingTimer.start();

    Logger::instance().log(LogLevel::Info,
                           QStringLiteral("%1 established with %2")
                               .arg(qobject_cast<QSslSocket*>(m_socket) ? QStringLiteral("TLS session")
                                                                        : QStringLiteral("Client connection"))
                               .arg(peerAddress()));
}
