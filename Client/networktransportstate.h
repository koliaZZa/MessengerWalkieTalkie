#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QStringList>

struct TrackedClientPacket {
    QJsonObject payload;
    int retries = 0;
    qint64 retryAtMs = 0;
};

struct TrackedRetryActions {
    QList<QJsonObject> resendPackets;
    QStringList droppedMessageIds;
};

class NetworkTransportState
{
public:
    void startConnection(const QString& host, quint16 port);
    void prepareDisconnect();

    QString host() const;
    quint16 port() const;
    QByteArray& incomingBuffer();
    bool canReconnect() const;
    bool consumeManualDisconnect();
    int scheduleReconnect();

    void markConnected(qint64 nowMs);
    void markPongReceived(qint64 nowMs);
    bool isHeartbeatExpired(qint64 nowMs) const;

    quint32 nextOutgoingSequence();
    bool isDuplicateIncomingSequence(quint32 seq) const;
    void markIncomingSequence(quint32 seq);

    void trackTrackedPacket(const QString& id, const QJsonObject& packet, qint64 nowMs);
    void acknowledgeTrackedPacket(const QString& id);
    TrackedRetryActions collectTrackedRetryActions(qint64 nowMs);

    int pendingTrackedCount() const;
    qint64 lastPongAtMs() const;

private:
    static QString normalizedHost(const QString& host);
    static quint16 normalizedPort(quint16 port);

    QByteArray m_buffer;
    QHash<QString, TrackedClientPacket> m_trackedPackets;
    quint32 m_nextOutgoingSeq {0};
    quint32 m_lastIncomingSeq {0};
    qint64 m_lastPongAtMs {0};
    QString m_host {QStringLiteral("127.0.0.1")};
    quint16 m_port {5555};
    bool m_manualDisconnect {false};
    bool m_reconnectEnabled {true};
    int m_reconnectAttempt {0};
};
