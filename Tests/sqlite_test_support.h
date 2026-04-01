#pragma once

#include <functional>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace TestSupport {

inline QString uniqueConnectionName(const QString& prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

inline QString sharedMemoryDatabaseUri(const QString& prefix)
{
    return QStringLiteral("file:%1?mode=memory&cache=shared").arg(uniqueConnectionName(prefix));
}

class DatabaseConnection
{
public:
    DatabaseConnection() = default;
    ~DatabaseConnection()
    {
        close();
    }

    DatabaseConnection(const DatabaseConnection&) = delete;
    DatabaseConnection& operator=(const DatabaseConnection&) = delete;

    bool open(const QString& databasePath, const QString& prefix, QString* errorMessage = nullptr)
    {
        close();

        m_connectionName = uniqueConnectionName(prefix);
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        if (databasePath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            m_database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_URI"));
        }
        m_database.setDatabaseName(databasePath);
        if (m_database.open()) {
            return true;
        }

        const QString localError = QStringLiteral("%1 (path: %2)").arg(m_database.lastError().text(), databasePath);
        close();
        if (errorMessage) {
            *errorMessage = localError;
        }
        return false;
    }

    QSqlDatabase& database()
    {
        return m_database;
    }

    void close()
    {
        if (!m_database.isValid()) {
            return;
        }

        const QString connectionName = m_connectionName;
        m_database.close();
        m_database = QSqlDatabase();
        m_connectionName.clear();
        QSqlDatabase::removeDatabase(connectionName);
    }

private:
    QString m_connectionName;
    QSqlDatabase m_database;
};

inline bool execSql(const QString& databasePath,
                    const QString& prefix,
                    const std::function<bool(QSqlDatabase&)>& callback,
                    QString* errorMessage = nullptr)
{
    DatabaseConnection connection;
    if (!connection.open(databasePath, prefix, errorMessage)) {
        return false;
    }

    const bool ok = callback(connection.database());
    if (!ok && errorMessage && errorMessage->isEmpty()) {
        *errorMessage = connection.database().lastError().text();
    }
    return ok;
}

inline bool probeSqliteDriver(QString* errorMessage)
{
    const QString databasePath = sharedMemoryDatabaseUri(QStringLiteral("sqlite_probe"));
    return execSql(databasePath, QStringLiteral("probe_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT 1"))) {
            if (errorMessage) {
                *errorMessage = query.lastError().text();
            }
            return false;
        }

        const bool ok = query.next() && query.value(0).toInt() == 1;
        if (!ok && errorMessage) {
            *errorMessage = QStringLiteral("SQLite probe query returned unexpected result");
        }
        return ok;
    }, errorMessage);
}

}  // namespace TestSupport
