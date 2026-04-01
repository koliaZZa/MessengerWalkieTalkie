#include "networktransportstate.h"

#include <QtGlobal>

namespace {

constexpr int kRetryScheduleMs[] = {2000, 4000, 8000, 16000, 32000};
constexpr int kReconnectScheduleMs[] = {1000, 2000, 4000, 8000, 16000, 32000};
constexpr int kHeartbeatTimeoutMs = 15000;

}

void NetworkTransportState::startConnection(const QString& host, quint16 port)
{
    m_host = normalizedHost(host);
    m_port = normalizedPort(port);
    m_manualDisconnect = false;
    m_reconnectEnabled = true;
    m_reconnectAttempt = 0;
    m_buffer.clear();
    m_pendingMessages.clear();
    m_nextOutgoingSeq = 0;
    m_lastIncomingSeq = 0;
    m_lastPongAtMs = 0;
}

void NetworkTransportState::prepareDisconnect()
{
    m_manualDisconnect = true;
    m_reconnectEnabled = false;
    m_pendingMessages.clear();
    m_buffer.clear();
}

QString NetworkTransportState::host() const
{
    return m_host;
}

quint16 NetworkTransportState::port() const
{
    return m_port;
}

QByteArray& NetworkTransportState::incomingBuffer()
{
    return m_buffer;
}

bool NetworkTransportState::canReconnect() const
{
    return m_reconnectEnabled && !m_manualDisconnect;
}

bool NetworkTransportState::consumeManualDisconnect()
{
    if (!m_manualDisconnect) {
        return false;
    }

    m_manualDisconnect = false;
    return true;
}

int NetworkTransportState::scheduleReconnect()
{
    if (!canReconnect()) {
        return -1;
    }

    const int scheduleSize = static_cast<int>(sizeof(kReconnectScheduleMs) / sizeof(kReconnectScheduleMs[0]));
    const int index = qMin(m_reconnectAttempt, scheduleSize - 1);
    const int delayMs = kReconnectScheduleMs[index];
    ++m_reconnectAttempt;
    return delayMs;
}

void NetworkTransportState::markConnected(qint64 nowMs)
{
    m_reconnectAttempt = 0;
    m_lastPongAtMs = nowMs;
}

void NetworkTransportState::markPongReceived(qint64 nowMs)
{
    m_lastPongAtMs = nowMs;
}

bool NetworkTransportState::isHeartbeatExpired(qint64 nowMs) const
{
    return m_lastPongAtMs > 0 && nowMs - m_lastPongAtMs > kHeartbeatTimeoutMs;
}

quint32 NetworkTransportState::nextOutgoingSequence()
{
    return ++m_nextOutgoingSeq;
}

bool NetworkTransportState::isDuplicateIncomingSequence(quint32 seq) const
{
    return seq > 0 && seq <= m_lastIncomingSeq;
}

void NetworkTransportState::markIncomingSequence(quint32 seq)
{
    if (seq > m_lastIncomingSeq) {
        m_lastIncomingSeq = seq;
    }
}

void NetworkTransportState::trackReliableMessage(const QString& id, const QJsonObject& packet, qint64 nowMs)
{
    PendingClientMessage pending;
    pending.payload = packet;
    pending.retryAtMs = nowMs + kRetryScheduleMs[0];
    m_pendingMessages.insert(id, pending);
}

void NetworkTransportState::acknowledgeReliableMessage(const QString& id)
{
    m_pendingMessages.remove(id);
}

RetryActions NetworkTransportState::collectRetryActions(qint64 nowMs)
{
    RetryActions actions;

    for (auto it = m_pendingMessages.begin(); it != m_pendingMessages.end();) {
        PendingClientMessage& pending = it.value();
        if (nowMs < pending.retryAtMs) {
            ++it;
            continue;
        }

        if (pending.retries >= 5) {
            actions.droppedMessageIds.append(it.key());
            it = m_pendingMessages.erase(it);
            continue;
        }

        const int delayIndex = qMin(pending.retries + 1, 4);
        pending.retryAtMs = nowMs + kRetryScheduleMs[delayIndex];
        ++pending.retries;
        actions.resendPackets.append(pending.payload);
        ++it;
    }

    return actions;
}

int NetworkTransportState::pendingReliableCount() const
{
    return m_pendingMessages.size();
}

qint64 NetworkTransportState::lastPongAtMs() const
{
    return m_lastPongAtMs;
}

QString NetworkTransportState::normalizedHost(const QString& host)
{
    const QString trimmed = host.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("127.0.0.1") : trimmed;
}

quint16 NetworkTransportState::normalizedPort(quint16 port)
{
    return port == 0 ? 5555 : port;
}
