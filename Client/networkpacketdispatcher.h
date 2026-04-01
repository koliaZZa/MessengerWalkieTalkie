#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

class NetworkPacketDispatcher : public QObject
{
    Q_OBJECT

public:
    enum class DispatchResult {
        Handled,
        InvalidPacket,
        Unhandled
    };

    explicit NetworkPacketDispatcher(QObject* parent = nullptr);

    DispatchResult dispatch(const QJsonObject& packet, QString* errorMessage = nullptr);

signals:
    void authSucceeded(const QString& username, const QString& sessionToken, qint64 sessionExpiresAt);
    void authFailed(const QString& message);
    void sessionInvalid(const QString& message);
    void usersUpdated(const QStringList& users);
    void dialogsReceived(const QStringList& dialogs);
    void historyReceived(const QString& chatUser, const QJsonArray& items);
    void userLookupFinished(const QString& username, bool exists, bool online);
    void publicMessageReceived(const QString& id, const QString& from, const QString& text, qint64 createdAt);
    void privateMessageReceived(const QString& id, const QString& from, const QString& text, qint64 createdAt);
    void messageQueued(const QString& id, const QString& to, qint64 createdAt);
    void messageDelivered(const QString& id, qint64 createdAt);
    void messageRead(const QString& id, const QString& from);
    void transportError(const QString& message);
};
