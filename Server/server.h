#pragma once

#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include <QMap>

enum class ClientState {
    Connected,
    Authenticated
};

struct ClientSession {
    QTcpSocket* socket;
    QString username;
    ClientState state = ClientState::Connected;
};

class MessengerServer : public QObject
{
    Q_OBJECT

public:
    explicit MessengerServer(QObject *parent = nullptr);
    ~MessengerServer();

    bool start(quint16 port);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();

private:
    void processMessage(ClientSession* client, const QJsonObject& obj);
    void handleRegister(ClientSession* client, const QJsonObject& obj);
    void handleLogin(ClientSession* client, const QJsonObject& obj);
    void handleChatMessage(ClientSession* client, const QJsonObject& obj);
    void handleBroadcast(ClientSession* client, const QJsonObject& obj);

    void sendJson(QTcpSocket* socket, const QJsonObject& obj);
    void sendError(QTcpSocket* socket, const QString& message);

    QString hashPassword(const QString& password);

    void loadUsers();
    void saveUsers();

private:
    QTcpServer server;

    QMap<QTcpSocket*, ClientSession*> sessions;
    QMap<QString, ClientSession*> activeUsers;   // online users
    QMap<QString, QString> registeredUsers;      // username -> hash
};
