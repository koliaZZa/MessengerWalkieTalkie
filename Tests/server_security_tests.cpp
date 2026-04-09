#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

#include "sqlite_test_support.h"
#include "../Server/serverstorage.h"
#include "../Shared/authprotocol.h"

namespace {

QString legacyHash(const QString& password)
{
    return QString::fromUtf8(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
}

}  // namespace

class ServerSecurityTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registeredPasswordHashUsesPbkdf2Format();
    void legacyPasswordHashesAreRejected();
    void sessionIsIssuedRotatedAndInvalidated();
    void broadcastUpsertPreservesCreatedAt();
    void privateUpsertPreservesCreatedAndDeliveryTimestamps();
    void validatorsRejectBadInputs();
};

void ServerSecurityTests::initTestCase()
{
    QString probeError;
    QVERIFY2(QSqlDatabase::drivers().contains(QStringLiteral("QSQLITE")),
             qPrintable(QStringLiteral("Available SQL drivers: %1")
                            .arg(QSqlDatabase::drivers().join(QStringLiteral(", ")))));
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));
}

void ServerSecurityTests::registeredPasswordHashUsesPbkdf2Format()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("server_auth"));

    ServerStorage service;
    QVERIFY(service.init(databasePath));

    QString errorMessage;
    AuthProtocol::SessionInfo sessionInfo;
    QVERIFY2(service.registerUser(QStringLiteral("pbkdf_user"),
                                  QStringLiteral("supersecret"),
                                  errorMessage,
                                  &sessionInfo),
             qPrintable(errorMessage));
    QVERIFY(sessionInfo.isValid());

    QString storedHash;
    QVERIFY(TestSupport::execSql(databasePath, QStringLiteral("hash_check_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT password_hash FROM users WHERE username = 'pbkdf_user'"))) {
            return false;
        }
        if (!query.next()) {
            return false;
        }
        storedHash = query.value(0).toString();
        return true;
    }));

    QVERIFY(storedHash.startsWith(QStringLiteral("pbkdf2_sha256$")));
    QVERIFY(storedHash != legacyHash(QStringLiteral("supersecret")));
}

void ServerSecurityTests::legacyPasswordHashesAreRejected()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("server_legacy_auth"));

    ServerStorage service;
    QVERIFY(service.init(databasePath));

    QVERIFY(TestSupport::execSql(databasePath, QStringLiteral("legacy_insert_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("INSERT INTO users(username, password_hash) VALUES(?, ?)"));
        query.addBindValue(QStringLiteral("legacy_user"));
        query.addBindValue(legacyHash(QStringLiteral("legacy-pass")));
        return query.exec();
    }));

    QString errorMessage;
    AuthProtocol::SessionInfo sessionInfo;
    QVERIFY(!service.loginUser(QStringLiteral("legacy_user"),
                               QStringLiteral("legacy-pass"),
                               errorMessage,
                               &sessionInfo));
    QCOMPARE(errorMessage, QStringLiteral("Wrong password"));
    QVERIFY(!sessionInfo.isValid());
}

void ServerSecurityTests::sessionIsIssuedRotatedAndInvalidated()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("server_session"));

    ServerStorage service;
    QVERIFY(service.init(databasePath));

    QString errorMessage;
    AuthProtocol::SessionInfo initialSession;
    QVERIFY2(service.registerUser(QStringLiteral("session_user"),
                                  QStringLiteral("supersecret"),
                                  errorMessage,
                                  &initialSession),
             qPrintable(errorMessage));
    QVERIFY(initialSession.isValid());

    AuthProtocol::SessionInfo resumedSession;
    QVERIFY2(service.resumeSession(QStringLiteral("session_user"),
                                   initialSession.token,
                                   errorMessage,
                                   &resumedSession),
             qPrintable(errorMessage));
    QVERIFY(resumedSession.isValid());
    QVERIFY(resumedSession.token != initialSession.token);
    QVERIFY(resumedSession.expiresAt >= initialSession.expiresAt);

    AuthProtocol::SessionInfo staleSession;
    QVERIFY(!service.resumeSession(QStringLiteral("session_user"),
                                   initialSession.token,
                                   errorMessage,
                                   &staleSession));
    QVERIFY(!errorMessage.isEmpty());

    QVERIFY(service.invalidateSession(QStringLiteral("session_user")));
    QVERIFY(!service.resumeSession(QStringLiteral("session_user"),
                                   resumedSession.token,
                                   errorMessage,
                                   &staleSession));
}

void ServerSecurityTests::broadcastUpsertPreservesCreatedAt()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("server_broadcast_upsert"));

    ServerStorage service;
    QVERIFY(service.init(databasePath));

    QVERIFY(service.storeBroadcastMessage(QStringLiteral("broadcast-1"),
                                         QStringLiteral("alice"),
                                         QStringLiteral("hello"),
                                         1000));
    QVERIFY(service.storeBroadcastMessage(QStringLiteral("broadcast-1"),
                                         QStringLiteral("alice"),
                                         QStringLiteral("updated"),
                                         5000));

    QString storedText;
    qint64 storedCreatedAt = 0;
    QVERIFY(TestSupport::execSql(databasePath, QStringLiteral("broadcast_upsert_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "SELECT text, created_at FROM broadcast_messages WHERE id = 'broadcast-1'"))) {
            return false;
        }
        if (!query.next()) {
            return false;
        }
        storedText = query.value(0).toString();
        storedCreatedAt = query.value(1).toLongLong();
        return true;
    }));

    QCOMPARE(storedText, QStringLiteral("updated"));
    QCOMPARE(storedCreatedAt, 1000);
}

void ServerSecurityTests::privateUpsertPreservesCreatedAndDeliveryTimestamps()
{
    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("server_private_upsert"));

    ServerStorage service;
    QVERIFY(service.init(databasePath));

    QVERIFY(service.storePrivateMessage(QStringLiteral("private-1"),
                                        QStringLiteral("alice"),
                                        QStringLiteral("bob"),
                                        QStringLiteral("hello"),
                                        1000));
    QVERIFY(service.markMessageDelivered(QStringLiteral("private-1"), 2000));
    QVERIFY(service.markMessageRead(QStringLiteral("private-1"), 3000));
    QVERIFY(service.storePrivateMessage(QStringLiteral("private-1"),
                                        QStringLiteral("alice"),
                                        QStringLiteral("bob"),
                                        QStringLiteral("updated"),
                                        5000));

    QString storedText;
    qint64 storedCreatedAt = 0;
    qint64 storedDeliveredAt = 0;
    qint64 storedReadAt = 0;
    QVERIFY(TestSupport::execSql(databasePath, QStringLiteral("private_upsert_"), [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "SELECT text, created_at, delivered_at, read_at FROM messages WHERE id = 'private-1'"))) {
            return false;
        }
        if (!query.next()) {
            return false;
        }
        storedText = query.value(0).toString();
        storedCreatedAt = query.value(1).toLongLong();
        storedDeliveredAt = query.value(2).toLongLong();
        storedReadAt = query.value(3).toLongLong();
        return true;
    }));

    QCOMPARE(storedText, QStringLiteral("updated"));
    QCOMPARE(storedCreatedAt, 1000);
    QCOMPARE(storedDeliveredAt, 2000);
    QCOMPARE(storedReadAt, 3000);
}

void ServerSecurityTests::validatorsRejectBadInputs()
{
    QVERIFY(AuthProtocol::isAsciiUsernameValid(QStringLiteral("valid_user_01")));
    QVERIFY(!AuthProtocol::isAsciiUsernameValid(QStringLiteral("bad-user")));
    QVERIFY(!AuthProtocol::isAsciiUsernameValid(QStringLiteral("????")));
    QVERIFY(!AuthProtocol::isAsciiUsernameValid(QStringLiteral("ab")));

    QVERIFY(AuthProtocol::isMessageTextValid(QStringLiteral("hello")));
    QVERIFY(!AuthProtocol::isMessageTextValid(QString()));
    QVERIFY(!AuthProtocol::isMessageTextValid(QStringLiteral("   ")));
    QVERIFY(!AuthProtocol::isMessageTextValid(QString(AuthProtocol::kMaxMessageLength + 1, QChar(u'a'))));

    QVERIFY(AuthProtocol::isPrivateRecipientValid(QStringLiteral("alice"), QStringLiteral("bob")));
    QVERIFY(!AuthProtocol::isPrivateRecipientValid(QStringLiteral("alice"), QStringLiteral("alice")));

    const QString databasePath = TestSupport::sharedMemoryDatabaseUri(QStringLiteral("server_validation"));

    ServerStorage service;
    QVERIFY(service.init(databasePath));

    QString errorMessage;
    AuthProtocol::SessionInfo sessionInfo;
    QVERIFY(!service.registerUser(QStringLiteral("bad-user"),
                                  QStringLiteral("supersecret"),
                                  errorMessage,
                                  &sessionInfo));
    QVERIFY(errorMessage.contains(QStringLiteral("Username")));

    QVERIFY(!service.registerUser(QStringLiteral("valid_user"),
                                  QStringLiteral("short"),
                                  errorMessage,
                                  &sessionInfo));
    QVERIFY(errorMessage.contains(QStringLiteral("Password")));
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("server_security_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    ServerSecurityTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "server_security_tests.moc"
