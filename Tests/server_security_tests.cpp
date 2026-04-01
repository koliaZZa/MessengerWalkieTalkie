#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

#include "sqlite_test_support.h"
#include "../Server/authservice.h"
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

    AuthService service;
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

    AuthService service;
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

    AuthService service;
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

    AuthService service;
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
