#include "server.h"
#include <QDebug>

MessengerServer::MessengerServer(QObject *parent)
    : QObject(parent)
{
    connect(&server, &QTcpServer::newConnection,
            this, &MessengerServer::onNewConnection);

    loadUsers();
}

MessengerServer::~MessengerServer()
{
    saveUsers();

    for (auto* session : sessions) {
        session->socket->close();
        delete session;
    }
}

bool MessengerServer::start(quint16 port)
{
    if (!server.listen(QHostAddress::Any, port)) {
        qCritical() << "Server failed to start";
        return false;
    }

    qDebug() << "Server started on port" << port;
    return true;
}

void MessengerServer::onNewConnection()
{
    QTcpSocket* socket = server.nextPendingConnection();

    ClientSession* session = new ClientSession;
    session->socket = socket;

    sessions[socket] = session;

    connect(socket, &QTcpSocket::readyRead,
            this, &MessengerServer::onReadyRead);

    connect(socket, &QTcpSocket::disconnected,
            this, &MessengerServer::onClientDisconnected);

    qDebug() << "New client connected";
}

void MessengerServer::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    ClientSession* session = sessions[socket];

    while (socket->canReadLine()) {
        QByteArray data = socket->readLine().trimmed();

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(data, &error);

        if (error.error != QJsonParseError::NoError) {
            sendError(socket, "Invalid JSON");
            return;
        }

        processMessage(session, doc.object());
    }
}

void MessengerServer::processMessage(ClientSession* client, const QJsonObject& obj)
{
    QString type = obj["type"].toString();

    if (type == "register")
        handleRegister(client, obj);
    else if (type == "login")
        handleLogin(client, obj);
    else if (type == "message")
        handleChatMessage(client, obj);
    else if (type == "broadcast")
        handleBroadcast(client, obj);
    else
        sendError(client->socket, "Unknown message type");
}

void MessengerServer::handleRegister(ClientSession* client, const QJsonObject& obj)
{
    QString username = obj["username"].toString();
    QString password = obj["password"].toString();

    if (registeredUsers.contains(username)) {
        sendError(client->socket, "User already exists");
        return;
    }

    registeredUsers[username] = hashPassword(password);
    saveUsers();

    sendJson(client->socket, {{"type", "auth_ok"}});
}

void MessengerServer::handleLogin(ClientSession* client, const QJsonObject& obj)
{
    QString username = obj["username"].toString();
    QString password = obj["password"].toString();

    if (!registeredUsers.contains(username) ||
        registeredUsers[username] != hashPassword(password)) {

        sendJson(client->socket, {{"type", "auth_fail"}});
        return;
    }

    client->state = ClientState::Authenticated;
    client->username = username;
    activeUsers[username] = client;

    sendJson(client->socket, {{"type", "auth_ok"}});
}

void MessengerServer::handleChatMessage(ClientSession* client, const QJsonObject& obj)
{
    if (client->state != ClientState::Authenticated) {
        sendError(client->socket, "Not authenticated");
        return;
    }

    QString to = obj["to"].toString();
    QString text = obj["text"].toString();

    if (!activeUsers.contains(to)) {
        sendError(client->socket, "User not online");
        return;
    }

    QJsonObject message{
        {"type", "message"},
        {"from", client->username},
        {"text", text}
    };

    sendJson(activeUsers[to]->socket, message);
}

void MessengerServer::handleBroadcast(ClientSession* client, const QJsonObject& obj)
{
    if (client->state != ClientState::Authenticated) {
        sendError(client->socket, "Not authenticated");
        return;
    }

    QString text = obj["text"].toString();

    QJsonObject message{
        {"type", "broadcast"},
        {"from", client->username},
        {"text", text}
    };

    for (auto& user : activeUsers)
        sendJson(user->socket, message);
}

void MessengerServer::onClientDisconnected()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    ClientSession* session = sessions[socket];

    if (session->state == ClientState::Authenticated)
        activeUsers.remove(session->username);

    sessions.remove(socket);
    socket->deleteLater();
    delete session;

    qDebug() << "Client disconnected";
}

void MessengerServer::sendJson(QTcpSocket* socket, const QJsonObject& obj)
{
    QJsonDocument doc(obj);
    socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
}

void MessengerServer::sendError(QTcpSocket* socket, const QString& message)
{
    sendJson(socket, {{"type", "error"}, {"message", message}});
}

QString MessengerServer::hashPassword(const QString& password)
{
    QByteArray hash = QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256);
    return hash.toHex();
}

void MessengerServer::loadUsers()
{
    QFile file("users.json");
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject obj = doc.object();

    for (auto it = obj.begin(); it != obj.end(); ++it)
        registeredUsers[it.key()] = it.value().toString();
}

void MessengerServer::saveUsers()
{
    QFile file("users.json");
    if (!file.open(QIODevice::WriteOnly))
        return;

    QJsonObject obj;
    for (auto it = registeredUsers.begin(); it != registeredUsers.end(); ++it)
        obj[it.key()] = it.value();

    file.write(QJsonDocument(obj).toJson());
}
