#include "authservice.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

AuthService::AuthService(QObject* parent)
    : QObject(parent)
{
}

AuthService::~AuthService()
{
    if (m_database.isValid()) {
        const QString connectionName = m_connectionName;
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool AuthService::init(const QString& databasePath)
{
    if (m_database.isValid() && m_database.isOpen()) {
        return true;
    }

    m_connectionName = QStringLiteral("auth_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(databasePath);
    if (!m_database.open()) {
        return false;
    }

    return ensureSchema();
}

bool AuthService::registerUser(const QString& username, const QString& password, QString& errorMessage)
{
    const QString normalizedUsername = username.trimmed();
    if (!isUsernameValid(normalizedUsername)) {
        errorMessage = QStringLiteral("Username must be 3-32 latin letters, digits or underscore");
        return false;
    }

    if (password.size() < 4) {
        errorMessage = QStringLiteral("Password must be at least 4 characters");
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO users(username, password_hash) VALUES(?, ?)"));
    query.addBindValue(normalizedUsername);
    query.addBindValue(hashPassword(password));

    if (!query.exec()) {
        errorMessage = query.lastError().text().contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                           ? QStringLiteral("User already exists")
                           : QStringLiteral("Database error");
        return false;
    }

    return true;
}

bool AuthService::loginUser(const QString& username, const QString& password, QString& errorMessage) const
{
    const QString normalizedUsername = username.trimmed();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT password_hash FROM users WHERE username = ?"));
    query.addBindValue(normalizedUsername);

    if (!query.exec()) {
        errorMessage = QStringLiteral("Database error");
        return false;
    }

    if (!query.next()) {
        errorMessage = QStringLiteral("User not found");
        return false;
    }

    if (query.value(0).toString() != hashPassword(password)) {
        errorMessage = QStringLiteral("Wrong password");
        return false;
    }

    return true;
}

bool AuthService::userExists(const QString& username) const
{
    const QString normalizedUsername = username.trimmed();
    if (normalizedUsername.isEmpty()) {
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM users WHERE username = ?"));
    query.addBindValue(normalizedUsername);
    if (!query.exec()) {
        return false;
    }

    return query.next();
}

bool AuthService::storeBroadcastMessage(const QString& id,
                                        const QString& from,
                                        const QString& text,
                                        qint64 createdAt)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO broadcast_messages(id, from_user, text, created_at) "
        "VALUES(?, ?, ?, COALESCE((SELECT created_at FROM broadcast_messages WHERE id = ?), ?))"));
    query.addBindValue(id);
    query.addBindValue(from);
    query.addBindValue(text);
    query.addBindValue(id);
    query.addBindValue(createdAt);
    return query.exec();
}

bool AuthService::storePrivateMessage(const QString& id,
                                      const QString& from,
                                      const QString& to,
                                      const QString& text,
                                      qint64 createdAt)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO messages(id, from_user, to_user, text, created_at, delivered_at, read_at) "
        "VALUES(?, ?, ?, ?, ?, "
        "COALESCE((SELECT delivered_at FROM messages WHERE id = ?), NULL), "
        "COALESCE((SELECT read_at FROM messages WHERE id = ?), NULL))"));
    query.addBindValue(id);
    query.addBindValue(from);
    query.addBindValue(to);
    query.addBindValue(text);
    query.addBindValue(createdAt);
    query.addBindValue(id);
    query.addBindValue(id);
    return query.exec();
}

bool AuthService::markMessageDelivered(const QString& messageId, qint64 deliveredAtMs)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE messages SET delivered_at = COALESCE(delivered_at, ?) WHERE id = ?"));
    query.addBindValue(deliveredAtMs);
    query.addBindValue(messageId);
    return query.exec();
}

bool AuthService::markMessageRead(const QString& messageId, qint64 readAtMs)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE messages "
        "SET delivered_at = COALESCE(delivered_at, ?), read_at = ? "
        "WHERE id = ?"));
    query.addBindValue(readAtMs);
    query.addBindValue(readAtMs);
    query.addBindValue(messageId);
    return query.exec();
}

QStringList AuthService::loadDialogUsers(const QString& username) const
{
    QStringList dialogs;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT DISTINCT dialog_user FROM ("
        "SELECT to_user AS dialog_user FROM messages WHERE from_user = ? "
        "UNION "
        "SELECT from_user AS dialog_user FROM messages WHERE to_user = ?"
        ") ORDER BY lower(dialog_user), dialog_user"));
    query.addBindValue(username);
    query.addBindValue(username);
    if (!query.exec()) {
        return dialogs;
    }

    while (query.next()) {
        const QString value = query.value(0).toString();
        if (!value.isEmpty()) {
            dialogs.append(value);
        }
    }

    return dialogs;
}

QList<HistoryMessageRecord> AuthService::loadBroadcastHistory(int limit) const
{
    QList<HistoryMessageRecord> messages;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, from_user, text, created_at "
        "FROM ("
        "SELECT id, from_user, text, created_at "
        "FROM broadcast_messages "
        "ORDER BY created_at DESC, id DESC "
        "LIMIT ?"
        ") ORDER BY created_at ASC, id ASC"));
    query.addBindValue(qMax(1, limit));

    if (!query.exec()) {
        return messages;
    }

    while (query.next()) {
        HistoryMessageRecord message;
        message.id = query.value(0).toString();
        message.from = query.value(1).toString();
        message.text = query.value(2).toString();
        message.createdAt = query.value(3).toLongLong();
        message.status = QStringLiteral("delivered");
        messages.append(message);
    }

    return messages;
}

QList<HistoryMessageRecord> AuthService::loadPrivateHistory(const QString& username,
                                                            const QString& otherUsername,
                                                            int limit) const
{
    QList<HistoryMessageRecord> messages;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, from_user, to_user, text, created_at, delivered_at, read_at "
        "FROM ("
        "SELECT id, from_user, to_user, text, created_at, delivered_at, read_at "
        "FROM messages "
        "WHERE (from_user = ? AND to_user = ?) OR (from_user = ? AND to_user = ?) "
        "ORDER BY created_at DESC, id DESC "
        "LIMIT ?"
        ") ORDER BY created_at ASC, id ASC"));
    query.addBindValue(username);
    query.addBindValue(otherUsername);
    query.addBindValue(otherUsername);
    query.addBindValue(username);
    query.addBindValue(qMax(1, limit));

    if (!query.exec()) {
        return messages;
    }

    while (query.next()) {
        HistoryMessageRecord message;
        message.id = query.value(0).toString();
        message.from = query.value(1).toString();
        message.to = query.value(2).toString();
        message.text = query.value(3).toString();
        message.createdAt = query.value(4).toLongLong();
        const bool isDelivered = !query.value(5).isNull();
        const bool isRead = !query.value(6).isNull();
        message.status = isRead ? QStringLiteral("read")
                                : (isDelivered ? QStringLiteral("delivered")
                                               : QStringLiteral("sending"));
        messages.append(message);
    }

    return messages;
}

QList<PendingPrivateMessageRecord> AuthService::loadPendingPrivateMessages(const QString& username) const
{
    QList<PendingPrivateMessageRecord> messages;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, from_user, to_user, text, created_at "
        "FROM messages "
        "WHERE to_user = ? AND delivered_at IS NULL "
        "ORDER BY created_at ASC, id ASC"));
    query.addBindValue(username);

    if (!query.exec()) {
        return messages;
    }

    while (query.next()) {
        PendingPrivateMessageRecord message;
        message.id = query.value(0).toString();
        message.from = query.value(1).toString();
        message.to = query.value(2).toString();
        message.text = query.value(3).toString();
        message.createdAt = query.value(4).toLongLong();
        messages.append(message);
    }

    return messages;
}

bool AuthService::ensureSchema()
{
    QSqlQuery query(m_database);
    const QStringList statements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS users ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "username TEXT NOT NULL UNIQUE,"
            "password_hash TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS messages ("
            "id TEXT PRIMARY KEY,"
            "from_user TEXT NOT NULL,"
            "to_user TEXT NOT NULL,"
            "text TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "delivered_at INTEGER,"
            "read_at INTEGER)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS broadcast_messages ("
            "id TEXT PRIMARY KEY,"
            "from_user TEXT NOT NULL,"
            "text TEXT NOT NULL,"
            "created_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_from_to_time ON messages(from_user, to_user, created_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_to_from_time ON messages(to_user, from_user, created_at)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_messages_pending_delivery "
            "ON messages(to_user, delivered_at, created_at)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_broadcast_messages_created_at "
            "ON broadcast_messages(created_at)")
    };

    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            return false;
        }
    }

    return true;
}

QString AuthService::hashPassword(const QString& password) const
{
    return QString::fromUtf8(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool AuthService::isUsernameValid(const QString& username) const
{
    if (username.size() < 3 || username.size() > 32) {
        return false;
    }

    for (const QChar character : username) {
        if (!(character.isLetterOrNumber() || character == QChar(u'_'))) {
            return false;
        }
    }

    return true;
}
