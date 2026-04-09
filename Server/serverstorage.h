#pragma once

#include <QObject>
#include <QList>
#include <QSqlDatabase>
#include <QStringList>

#include "../Shared/authprotocol.h"

struct HistoryMessageRecord {
    QString id;
    QString from;
    QString to;
    QString text;
    QString status;
    qint64 createdAt = 0;
};

struct PendingPrivateMessageRecord {
    QString id;
    QString from;
    QString to;
    QString text;
    qint64 createdAt = 0;
};

class ServerStorage : public QObject
{
    Q_OBJECT

public:
    explicit ServerStorage(QObject* parent = nullptr);
    ~ServerStorage() override;

    bool init(const QString& databasePath = QStringLiteral("users.db"));
    bool registerUser(const QString& username,
                      const QString& password,
                      QString& errorMessage,
                      AuthProtocol::SessionInfo* sessionInfo = nullptr);
    bool loginUser(const QString& username,
                   const QString& password,
                   QString& errorMessage,
                   AuthProtocol::SessionInfo* sessionInfo = nullptr);
    bool resumeSession(const QString& username,
                       const QString& sessionToken,
                       QString& errorMessage,
                       AuthProtocol::SessionInfo* sessionInfo = nullptr);
    bool invalidateSession(const QString& username);
    bool userExists(const QString& username) const;
    bool storeBroadcastMessage(const QString& id,
                               const QString& from,
                               const QString& text,
                               qint64 createdAt);
    bool storePrivateMessage(const QString& id,
                             const QString& from,
                             const QString& to,
                             const QString& text,
                             qint64 createdAt);
    bool markMessageDelivered(const QString& messageId, qint64 deliveredAtMs);
    bool markMessageRead(const QString& messageId, qint64 readAtMs);
    QStringList loadDialogUsers(const QString& username) const;
    QList<HistoryMessageRecord> loadBroadcastHistory(int limit = 100) const;
    QList<HistoryMessageRecord> loadPrivateHistory(const QString& username,
                                                   const QString& otherUsername,
                                                   int limit = 100) const;
    QList<PendingPrivateMessageRecord> loadPendingPrivateMessages(const QString& username) const;

private:
    bool ensureSchema();
    QString hashPassword(const QString& password) const;
    bool verifyPassword(const QString& password, const QString& storedHash) const;
    QByteArray randomBytes(int count) const;
    QByteArray hmacSha256(const QByteArray& key, const QByteArray& message) const;
    QByteArray derivePbkdf2(const QByteArray& password,
                            const QByteArray& salt,
                            int iterations,
                            int keyLength) const;
    QString generateSessionToken() const;
    QString hashSessionToken(const QString& sessionToken) const;
    bool issueSession(const QString& username,
                      QString& errorMessage,
                      AuthProtocol::SessionInfo* sessionInfo = nullptr);
    bool removeExpiredSession(const QString& username);
    bool isUsernameValid(const QString& username) const;

    QString m_connectionName;
    QSqlDatabase m_database;
};
