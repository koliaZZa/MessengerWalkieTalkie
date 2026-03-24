#include "authservice.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
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

    QSqlQuery query(m_database);
    return query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT NOT NULL UNIQUE,"
        "password_hash TEXT NOT NULL)"));
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
