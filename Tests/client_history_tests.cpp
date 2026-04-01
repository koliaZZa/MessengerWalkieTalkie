#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

#include "sqlite_test_support.h"
#include "../Client/historystore.h"

namespace {

QStringList loadLastSessionColumns(const QString& databasePath)
{
    QStringList columns;
    TestSupport::execSql(databasePath, QStringLiteral("pragma_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("PRAGMA table_info(last_session)"))) {
            return false;
        }
        while (query.next()) {
            columns.append(query.value(1).toString());
        }
        return true;
    });
    return columns;
}

}  // namespace

class ClientHistoryTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void rejectsLegacyLastSessionSchema();
    void storesSessionTokenWithoutPasswordColumn();
};

void ClientHistoryTests::initTestCase()
{
    QString probeError;
    QVERIFY2(QSqlDatabase::drivers().contains(QStringLiteral("QSQLITE")),
             qPrintable(QStringLiteral("Available SQL drivers: %1")
                            .arg(QSqlDatabase::drivers().join(QStringLiteral(", ")))));
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));
}

void ClientHistoryTests::rejectsLegacyLastSessionSchema()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("client_history"));

    QString connectionError;
    TestSupport::DatabaseConnection keeper;
    QVERIFY2(keeper.open(databasePath, QStringLiteral("legacy_setup_"), &connectionError),
             qPrintable(connectionError));

    QSqlQuery query(keeper.database());
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TABLE last_session("
        "id INTEGER PRIMARY KEY CHECK(id = 1),"
        "username TEXT NOT NULL,"
        "password TEXT NOT NULL DEFAULT '',"
        "host TEXT NOT NULL,"
        "port INTEGER NOT NULL)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO last_session(id, username, password, host, port) "
        "VALUES(1, 'alice', 'plaintext-secret', '127.0.0.1', 5555)")));

    HistoryStore store;
    QVERIFY(!store.init(databasePath));

    const QStringList columns = loadLastSessionColumns(databasePath);
    QVERIFY(columns.contains(QStringLiteral("password")));
    QVERIFY(!columns.contains(QStringLiteral("session_token")));
    QVERIFY(!columns.contains(QStringLiteral("session_expires_at")));
}

void ClientHistoryTests::storesSessionTokenWithoutPasswordColumn()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("client_session"));

    HistoryStore store;
    QVERIFY(store.init(databasePath));

    LastSessionInfo session;
    session.username = QStringLiteral("bob");
    session.host = QStringLiteral("192.168.0.20");
    session.port = 6000;
    session.sessionToken = QStringLiteral("token-123");
    session.sessionExpiresAt = 123456789;
    store.saveLastSession(session);

    const LastSessionInfo loaded = store.loadLastSession();
    QCOMPARE(loaded.username, session.username);
    QCOMPARE(loaded.host, session.host);
    QCOMPARE(loaded.port, session.port);
    QCOMPARE(loaded.sessionToken, session.sessionToken);
    QCOMPARE(loaded.sessionExpiresAt, session.sessionExpiresAt);

    QString storedToken;
    qint64 storedExpiresAt = 0;
    QVERIFY(TestSupport::execSql(databasePath, QStringLiteral("session_check_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT session_token, session_expires_at FROM last_session WHERE id = 1"))) {
            return false;
        }
        if (!query.next()) {
            return false;
        }
        storedToken = query.value(0).toString();
        storedExpiresAt = query.value(1).toLongLong();
        return true;
    }));

    QCOMPARE(storedToken, session.sessionToken);
    QCOMPARE(storedExpiresAt, session.sessionExpiresAt);
    QVERIFY(!loadLastSessionColumns(databasePath).contains(QStringLiteral("password")));
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("client_history_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    ClientHistoryTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "client_history_tests.moc"
