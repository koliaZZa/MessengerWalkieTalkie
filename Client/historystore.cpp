#include "historystore.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>
#include <QUuid>

HistoryStore::HistoryStore(QObject* parent)
    : QObject(parent)
{
}

HistoryStore::~HistoryStore()
{
    if (m_database.isValid()) {
        const QString connectionName = m_connectionName;
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool HistoryStore::init(const QString& databasePath)
{
    if (m_database.isValid() && m_database.isOpen()) {
        return true;
    }

    const QString finalPath = databasePath.isEmpty() ? defaultDatabasePath() : databasePath;
    QFileInfo info(finalPath);
    QDir().mkpath(info.dir().absolutePath());

    m_connectionName = QStringLiteral("client_history_%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(finalPath);
    if (!m_database.open()) {
        return false;
    }

    return ensureSchema();
}

void HistoryStore::saveMessage(const QString& ownerUsername, const ChatMessage& message)
{
    if (!m_database.isOpen() || ownerUsername.isEmpty() || message.id.isEmpty()) {
        return;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO messages("
        "owner_username, message_id, chat_key, author, text, status, outgoing, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(ownerUsername);
    query.addBindValue(message.id);
    query.addBindValue(message.chatKey);
    query.addBindValue(message.author);
    query.addBindValue(message.text);
    query.addBindValue(message.status);
    query.addBindValue(message.outgoing ? 1 : 0);
    query.addBindValue(message.createdAt);
    query.exec();
}

QList<ChatMessage> HistoryStore::loadMessages(const QString& ownerUsername, const QString& chatKey) const
{
    QList<ChatMessage> messages;
    if (!m_database.isOpen()) {
        return messages;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT message_id, chat_key, author, text, status, outgoing, created_at "
        "FROM messages WHERE owner_username = ? AND chat_key = ? "
        "ORDER BY created_at ASC, message_id ASC"));
    query.addBindValue(ownerUsername);
    query.addBindValue(chatKey);
    if (!query.exec()) {
        return messages;
    }

    while (query.next()) {
        ChatMessage message;
        message.id = query.value(0).toString();
        message.chatKey = query.value(1).toString();
        message.author = query.value(2).toString();
        message.text = query.value(3).toString();
        message.status = query.value(4).toString();
        message.outgoing = query.value(5).toInt() != 0;
        message.createdAt = query.value(6).toLongLong();
        messages.append(message);
    }

    return messages;
}

QStringList HistoryStore::loadDialogs(const QString& ownerUsername) const
{
    QStringList dialogs;
    if (!m_database.isOpen()) {
        return dialogs;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT DISTINCT chat_key FROM messages "
        "WHERE owner_username = ? AND chat_key <> 'Broadcast' "
        "ORDER BY lower(chat_key), chat_key"));
    query.addBindValue(ownerUsername);
    if (!query.exec()) {
        return dialogs;
    }

    while (query.next()) {
        dialogs.append(query.value(0).toString());
    }

    return dialogs;
}

void HistoryStore::clearUserData(const QString& ownerUsername)
{
    if (!m_database.isOpen() || ownerUsername.isEmpty()) {
        return;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM messages WHERE owner_username = ?"));
    query.addBindValue(ownerUsername);
    query.exec();

    query.prepare(QStringLiteral("DELETE FROM last_session WHERE username = ?"));
    query.addBindValue(ownerUsername);
    query.exec();
}

void HistoryStore::saveLastSession(const LastSessionInfo& session)
{
    if (!m_database.isOpen() || !session.isValid()) {
        return;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO last_session(id, username, password, host, port) VALUES(1, ?, ?, ?, ?)"));
    query.addBindValue(session.username);
    query.addBindValue(session.password);
    query.addBindValue(session.host);
    query.addBindValue(static_cast<int>(session.port));
    query.exec();
}

LastSessionInfo HistoryStore::loadLastSession() const
{
    LastSessionInfo session;
    if (!m_database.isOpen()) {
        return session;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT username, password, host, port FROM last_session WHERE id = 1"));
    if (!query.exec() || !query.next()) {
        return session;
    }

    session.username = query.value(0).toString();
    session.password = query.value(1).toString();
    session.host = query.value(2).toString();
    session.port = static_cast<quint16>(query.value(3).toUInt());
    return session;
}

void HistoryStore::clearLastSession()
{
    if (!m_database.isOpen()) {
        return;
    }

    QSqlQuery query(m_database);
    query.exec(QStringLiteral("DELETE FROM last_session"));
}

QString HistoryStore::defaultDatabasePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/client_history.db");
}

bool HistoryStore::ensureSchema()
{
    QSqlQuery query(m_database);
    const QStringList statements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS messages("
            "owner_username TEXT NOT NULL,"
            "message_id TEXT PRIMARY KEY,"
            "chat_key TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "text TEXT NOT NULL,"
            "status TEXT NOT NULL,"
            "outgoing INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_client_messages_owner_chat_time "
            "ON messages(owner_username, chat_key, created_at)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS last_session("
            "id INTEGER PRIMARY KEY CHECK(id = 1),"
            "username TEXT NOT NULL,"
            "password TEXT NOT NULL DEFAULT '',"
            "host TEXT NOT NULL,"
            "port INTEGER NOT NULL)")
    };

    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            return false;
        }
    }

    if (!query.exec(QStringLiteral("PRAGMA table_info(last_session)"))) {
        return false;
    }

    bool hasPasswordColumn = false;
    while (query.next()) {
        if (query.value(1).toString() == QStringLiteral("password")) {
            hasPasswordColumn = true;
            break;
        }
    }

    if (!hasPasswordColumn
        && !query.exec(QStringLiteral(
            "ALTER TABLE last_session ADD COLUMN password TEXT NOT NULL DEFAULT ''"))) {
        return false;
    }

    return true;
}
