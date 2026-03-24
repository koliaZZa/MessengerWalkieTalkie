#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

struct PendingClientMessage {
    QJsonObject payload;
    int retries = 0;
    qint64 retryAtMs = 0;
};

class NetworkClient : public QObject
{
    Q_OBJECT

public:
    explicit NetworkClient(QObject* parent = nullptr);

    void connectToServer(const QString& host, quint16 port);
    void disconnectFromServer();
    bool isConnected() const;

    void login(const QString& username, const QString& password);
    void registerUser(const QString& username, const QString& password);
    void checkUserExists(const QString& username);
    QString sendBroadcastMessage(const QString& text);
    QString sendPrivateMessage(const QString& recipient, const QString& text);
    void sendReadReceipt(const QString& recipient, const QString& messageId);

signals:
    void socketConnected();
    void socketDisconnected();
    void authSucceeded(const QString& username);
    void authFailed(const QString& message);
    void usersUpdated(const QStringList& users);
    void userLookupFinished(const QString& username, bool exists, bool online);
    void publicMessageReceived(const QString& id, const QString& from, const QString& text);
    void privateMessageReceived(const QString& id, const QString& from, const QString& text);
    void messageDelivered(const QString& id);
    void messageRead(const QString& id, const QString& from);
    void transportError(const QString& message);
    void statusChanged(const QString& message);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void sendPing();
    void retryPendingMessages();
    void reconnectIfNeeded();

private:
    void sendUnreliable(const QJsonObject& packet);
    QString sendReliable(QJsonObject packet);
    void processPacket(const QJsonObject& packet);
    void sendAck(const QString& id, quint32 seq);
    qint64 nowMs() const;

    QTcpSocket m_socket;
    QByteArray m_buffer;
    QTimer m_pingTimer;
    QTimer m_retryTimer;
    QTimer m_reconnectTimer;
    QHash<QString, PendingClientMessage> m_pendingMessages;
    quint32 m_nextOutgoingSeq {0};
    quint32 m_lastIncomingSeq {0};
    qint64 m_lastPongAtMs {0};
    QString m_host {QStringLiteral("127.0.0.1")};
    quint16 m_port {5555};
    QString m_username;
    bool m_manualDisconnect {false};
};
