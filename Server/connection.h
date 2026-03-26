#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QTimer>
#include <QTcpSocket>

struct PendingMessage {
    QJsonObject payload;
    int retries = 0;
    qint64 retryAtMs = 0;
};

class Connection : public QObject
{
    Q_OBJECT

public:
    explicit Connection(QTcpSocket* socket, QObject* parent = nullptr);
    ~Connection() override;

    QString peerAddress() const;

signals:
    void packetReceived(Connection* connection, const QJsonObject& packet);
    void reliablePacketAcked(Connection* connection, const QJsonObject& packet);
    void closed(Connection* connection);

public slots:
    void start();
    void sendUnreliable(const QJsonObject& packet);
    void sendReliable(QJsonObject packet);
    void closeConnection();

private slots:
    void onReadyRead();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void checkPendingMessages();
    void sendPing();

private:
    void processIncomingPacket(const QJsonObject& packet);
    void sendAck(const QString& messageId, quint32 seq);
    qint64 nowMs() const;

    QTcpSocket* m_socket {nullptr};
    QByteArray m_buffer;
    QHash<QString, PendingMessage> m_pendingMessages;
    QTimer m_retryTimer;
    QTimer m_pingTimer;
    quint32 m_nextOutgoingSeq {0};
    quint32 m_lastIncomingSeq {0};
    qint64 m_lastPongAtMs {0};
};

Q_DECLARE_METATYPE(Connection*)
