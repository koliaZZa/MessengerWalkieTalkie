#pragma once

#include <QObject>
#include <QList>
#include <QSqlDatabase>
#include <QStringList>

#include "../Shared/authprotocol.h"

struct ChatMessage {
    QString id;
    QString chatKey;
    QString author;
    QString text;
    QString status;
    bool outgoing = false;
    qint64 createdAt = 0;
};

struct LastSessionInfo {
    QString username;
    QString host;
    quint16 port = 0;
    QString sessionToken;
    qint64 sessionExpiresAt = 0;

    bool hasStoredIdentity() const
    {
        return !username.isEmpty() && !host.isEmpty() && port > 0;
    }

    bool hasSessionToken() const
    {
        return hasStoredIdentity() && !sessionToken.isEmpty() && sessionExpiresAt > 0;
    }

    bool isValid() const
    {
        return hasStoredIdentity();
    }
};

class HistoryStore : public QObject
{
    Q_OBJECT

public:
    explicit HistoryStore(QObject* parent = nullptr);
    ~HistoryStore() override;

    bool init(const QString& databasePath = QString());
    void saveMessage(const QString& ownerUsername, const ChatMessage& message);
    QList<ChatMessage> loadMessages(const QString& ownerUsername, const QString& chatKey) const;
    QStringList loadDialogs(const QString& ownerUsername) const;
    void clearUserData(const QString& ownerUsername);

    void saveLastSession(const LastSessionInfo& session);
    LastSessionInfo loadLastSession() const;
    void clearLastSession();

private:
    QString defaultDatabasePath() const;
    bool ensureSchema();

    QString m_connectionName;
    QSqlDatabase m_database;
};
