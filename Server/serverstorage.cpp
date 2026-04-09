#include "serverstorage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

namespace {

constexpr int kPbkdf2Iterations = 120000;
constexpr int kPbkdf2KeyLength = 32;
constexpr int kPasswordSaltBytes = 16;
constexpr int kHmacBlockSize = 64;
constexpr char kPbkdf2Prefix[] = "pbkdf2_sha256";

QByteArray toBigEndian(quint32 value)
{
    QByteArray bytes(4, '\0');
    bytes[0] = static_cast<char>((value >> 24) & 0xFF);
    bytes[1] = static_cast<char>((value >> 16) & 0xFF);
    bytes[2] = static_cast<char>((value >> 8) & 0xFF);
    bytes[3] = static_cast<char>(value & 0xFF);
    return bytes;
}

bool constantTimeEquals(const QByteArray& left, const QByteArray& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (int index = 0; index < left.size(); ++index) {
        diff |= static_cast<unsigned char>(left.at(index) ^ right.at(index));
    }

    return diff == 0;
}

}

ServerStorage::ServerStorage(QObject* parent)
    : QObject(parent)
{
}

ServerStorage::~ServerStorage()
{
    if (m_database.isValid()) {
        const QString connectionName = m_connectionName;
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool ServerStorage::init(const QString& databasePath)
{
    if (m_database.isValid() && m_database.isOpen()) {
        return true;
    }

    m_connectionName = QStringLiteral("storage_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    if (databasePath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        m_database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_URI"));
    }
    m_database.setDatabaseName(databasePath);
    if (!m_database.open()) {
        return false;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
        return false;
    }

    return ensureSchema();
}

bool ServerStorage::registerUser(const QString& username,
                                 const QString& password,
                                 QString& errorMessage,
                                 AuthProtocol::SessionInfo* sessionInfo)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    if (!isUsernameValid(normalizedUsername)) {
        errorMessage = QStringLiteral("Username must be 3-32 latin letters, digits or underscore");
        return false;
    }

    if (password.size() < AuthProtocol::kMinPasswordLength) {
        errorMessage = QStringLiteral("Password must be at least %1 characters")
                           .arg(AuthProtocol::kMinPasswordLength);
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

    return issueSession(normalizedUsername, errorMessage, sessionInfo);
}

bool ServerStorage::loginUser(const QString& username,
                              const QString& password,
                              QString& errorMessage,
                              AuthProtocol::SessionInfo* sessionInfo)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
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

    const QString storedHash = query.value(0).toString();
    if (!verifyPassword(password, storedHash)) {
        errorMessage = QStringLiteral("Wrong password");
        return false;
    }

    return issueSession(normalizedUsername, errorMessage, sessionInfo);
}

bool ServerStorage::resumeSession(const QString& username,
                                  const QString& sessionToken,
                                  QString& errorMessage,
                                  AuthProtocol::SessionInfo* sessionInfo)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    if (normalizedUsername.isEmpty() || sessionToken.isEmpty()) {
        errorMessage = QStringLiteral("Session expired. Please sign in again.");
        return false;
    }

    if (!removeExpiredSession(normalizedUsername)) {
        errorMessage = QStringLiteral("Database error");
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT token_hash, expires_at FROM sessions WHERE username = ?"));
    query.addBindValue(normalizedUsername);
    if (!query.exec()) {
        errorMessage = QStringLiteral("Database error");
        return false;
    }

    if (!query.next()) {
        errorMessage = QStringLiteral("Session expired. Please sign in again.");
        return false;
    }

    const qint64 expiresAt = query.value(1).toLongLong();
    if (expiresAt <= QDateTime::currentMSecsSinceEpoch()) {
        invalidateSession(normalizedUsername);
        errorMessage = QStringLiteral("Session expired. Please sign in again.");
        return false;
    }

    const QByteArray actualHash = hashSessionToken(sessionToken).toUtf8();
    const QByteArray storedHash = query.value(0).toString().toUtf8();
    if (!constantTimeEquals(actualHash, storedHash)) {
        errorMessage = QStringLiteral("Session invalid. Please sign in again.");
        return false;
    }

    return issueSession(normalizedUsername, errorMessage, sessionInfo);
}

bool ServerStorage::invalidateSession(const QString& username)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    if (normalizedUsername.isEmpty()) {
        return true;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE username = ?"));
    query.addBindValue(normalizedUsername);
    return query.exec();
}

bool ServerStorage::userExists(const QString& username) const
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
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

bool ServerStorage::storeBroadcastMessage(const QString& id,
                                          const QString& from,
                                          const QString& text,
                                          qint64 createdAt)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO broadcast_messages(id, from_user, text, created_at) "
        "VALUES(?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "from_user = excluded.from_user, "
        "text = excluded.text"));
    query.addBindValue(id);
    query.addBindValue(from);
    query.addBindValue(text);
    query.addBindValue(createdAt);
    return query.exec();
}

bool ServerStorage::storePrivateMessage(const QString& id,
                                        const QString& from,
                                        const QString& to,
                                        const QString& text,
                                        qint64 createdAt)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO messages(id, from_user, to_user, text, created_at, delivered_at, read_at) "
        "VALUES(?, ?, ?, ?, ?, NULL, NULL) "
        "ON CONFLICT(id) DO UPDATE SET "
        "from_user = excluded.from_user, "
        "to_user = excluded.to_user, "
        "text = excluded.text"));
    query.addBindValue(id);
    query.addBindValue(from);
    query.addBindValue(to);
    query.addBindValue(text);
    query.addBindValue(createdAt);
    return query.exec();
}

bool ServerStorage::markMessageDelivered(const QString& messageId, qint64 deliveredAtMs)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE messages SET delivered_at = COALESCE(delivered_at, ?) WHERE id = ?"));
    query.addBindValue(deliveredAtMs);
    query.addBindValue(messageId);
    return query.exec();
}

bool ServerStorage::markMessageRead(const QString& messageId, qint64 readAtMs)
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

QStringList ServerStorage::loadDialogUsers(const QString& username) const
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

QList<HistoryMessageRecord> ServerStorage::loadBroadcastHistory(int limit) const
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

QList<HistoryMessageRecord> ServerStorage::loadPrivateHistory(const QString& username,
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

QList<PendingPrivateMessageRecord> ServerStorage::loadPendingPrivateMessages(const QString& username) const
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

bool ServerStorage::ensureSchema()
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
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sessions ("
            "username TEXT PRIMARY KEY,"
            "token_hash TEXT NOT NULL,"
            "expires_at INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "FOREIGN KEY(username) REFERENCES users(username) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_from_to_time ON messages(from_user, to_user, created_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_to_from_time ON messages(to_user, from_user, created_at)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_messages_pending_delivery "
            "ON messages(to_user, delivered_at, created_at)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_broadcast_messages_created_at "
            "ON broadcast_messages(created_at)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_sessions_expires_at "
            "ON sessions(expires_at)")
    };

    for (auto& statement : statements) {
        if (!query.exec(statement)) {
            return false;
        }
    }

    return true;
}

QString ServerStorage::hashPassword(const QString& password) const
{
    const QByteArray salt = randomBytes(kPasswordSaltBytes);
    const QByteArray derived = derivePbkdf2(password.toUtf8(), salt, kPbkdf2Iterations, kPbkdf2KeyLength);
    return QStringLiteral("%1$%2$%3$%4")
        .arg(QString::fromLatin1(kPbkdf2Prefix))
        .arg(kPbkdf2Iterations)
        .arg(QString::fromUtf8(salt.toHex()))
        .arg(QString::fromUtf8(derived.toHex()));
}

bool ServerStorage::verifyPassword(const QString& password, const QString& storedHash) const
{
    const QStringList parts = storedHash.split(QStringLiteral("$"));
    if (parts.size() != 4 || parts.constFirst() != QString::fromLatin1(kPbkdf2Prefix)) {
        return false;
    }

    bool iterationsOk = false;
    const int iterations = parts.at(1).toInt(&iterationsOk);
    const QByteArray salt = QByteArray::fromHex(parts.at(2).toUtf8());
    const QByteArray expected = QByteArray::fromHex(parts.at(3).toUtf8());
    if (!iterationsOk || iterations <= 0 || salt.isEmpty() || expected.isEmpty()) {
        return false;
    }

    const QByteArray actual = derivePbkdf2(password.toUtf8(), salt, iterations, expected.size());
    return constantTimeEquals(actual, expected);
}

QByteArray ServerStorage::randomBytes(int count) const
{
    QByteArray bytes(count, '\0');
    for (int index = 0; index < count; ++index) {
        bytes[index] = static_cast<char>(QRandomGenerator::system()->generate() & 0xFF);
    }
    return bytes;
}

QByteArray ServerStorage::hmacSha256(const QByteArray& key, const QByteArray& message) const
{
    QByteArray normalizedKey = key;
    if (normalizedKey.size() > kHmacBlockSize) {
        normalizedKey = QCryptographicHash::hash(normalizedKey, QCryptographicHash::Sha256);
    }

    normalizedKey = normalizedKey.leftJustified(kHmacBlockSize, '\0', true);

    QByteArray innerPad(kHmacBlockSize, '\0');
    QByteArray outerPad(kHmacBlockSize, '\0');
    for (int index = 0; index < kHmacBlockSize; ++index) {
        const char keyByte = normalizedKey.at(index);
        innerPad[index] = static_cast<char>(keyByte ^ 0x36);
        outerPad[index] = static_cast<char>(keyByte ^ 0x5C);
    }

    QByteArray innerInput = innerPad;
    innerInput.append(message);
    const QByteArray innerHash = QCryptographicHash::hash(innerInput, QCryptographicHash::Sha256);

    QByteArray outerInput = outerPad;
    outerInput.append(innerHash);
    return QCryptographicHash::hash(outerInput, QCryptographicHash::Sha256);
}

QByteArray ServerStorage::derivePbkdf2(const QByteArray& password,
                                       const QByteArray& salt,
                                       int iterations,
                                       int keyLength) const
{
    QByteArray derived;
    quint32 blockIndex = 1;
    while (derived.size() < keyLength) {
        QByteArray initialBlock = salt;
        initialBlock.append(toBigEndian(blockIndex));

        QByteArray value = hmacSha256(password, initialBlock);
        QByteArray block = value;
        for (int round = 1; round < iterations; ++round) {
            value = hmacSha256(password, value);
            for (int byteIndex = 0; byteIndex < block.size(); ++byteIndex) {
                block[byteIndex] = static_cast<char>(block.at(byteIndex) ^ value.at(byteIndex));
            }
        }

        derived.append(block);
        ++blockIndex;
    }

    derived.truncate(keyLength);
    return derived;
}

QString ServerStorage::generateSessionToken() const
{
    return QString::fromUtf8(randomBytes(AuthProtocol::kSessionTokenBytes).toHex());
}

QString ServerStorage::hashSessionToken(const QString& sessionToken) const
{
    return QString::fromUtf8(QCryptographicHash::hash(sessionToken.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool ServerStorage::issueSession(const QString& username,
                                 QString& errorMessage,
                                 AuthProtocol::SessionInfo* sessionInfo)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    if (normalizedUsername.isEmpty()) {
        errorMessage = QStringLiteral("User not found");
        return false;
    }

    const QString sessionToken = generateSessionToken();
    const qint64 createdAt = QDateTime::currentMSecsSinceEpoch();
    const qint64 expiresAt = createdAt + AuthProtocol::kSessionLifetimeMs;

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sessions(username, token_hash, expires_at, created_at) "
        "VALUES(?, ?, ?, ?)"));
    query.addBindValue(normalizedUsername);
    query.addBindValue(hashSessionToken(sessionToken));
    query.addBindValue(expiresAt);
    query.addBindValue(createdAt);
    if (!query.exec()) {
        errorMessage = QStringLiteral("Database error");
        return false;
    }

    if (sessionInfo) {
        sessionInfo->token = sessionToken;
        sessionInfo->expiresAt = expiresAt;
    }

    return true;
}

bool ServerStorage::removeExpiredSession(const QString& username)
{
    const QString normalizedUsername = AuthProtocol::normalizeUsername(username);
    if (normalizedUsername.isEmpty()) {
        return true;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "DELETE FROM sessions WHERE username = ? AND expires_at <= ?"));
    query.addBindValue(normalizedUsername);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return query.exec();
}

bool ServerStorage::isUsernameValid(const QString& username) const
{
    return AuthProtocol::isAsciiUsernameValid(AuthProtocol::normalizeUsername(username));
}








