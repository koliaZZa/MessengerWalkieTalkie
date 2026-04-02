#include <memory>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "sqlite_test_support.h"
#include "tls_test_support.h"
#include "../Client/networkclient.h"
#include "../Server/server.h"

namespace {

bool waitForCount(QSignalSpy& spy, int expectedCount, int timeoutMs = 5000)
{
    if (spy.count() >= expectedCount) {
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    while (spy.count() < expectedCount && timer.elapsed() < timeoutMs) {
        const int remainingMs = qMax(1, timeoutMs - static_cast<int>(timer.elapsed()));
        spy.wait(qMin(100, remainingMs));
    }

    return spy.count() >= expectedCount;
}

std::unique_ptr<Server> startServerWithRetry(const QString& databasePath,
                                             const TlsConfiguration::ServerSettings& tlsSettings,
                                             quint16 port = 0,
                                             int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        auto server = std::make_unique<Server>();
        server->setDatabasePath(databasePath);
        server->setTlsConfiguration(tlsSettings);
        if (server->start(port)) {
            return server;
        }

        server.reset();
        QTest::qWait(50);
    }

    return {};
}

}  // namespace

class TlsIntegrationTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void networkClientCanAuthenticateResumeAndExchangeMessagesOverTls();
};

void TlsIntegrationTests::initTestCase()
{
    QString probeError;
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));

    TlsConfiguration::ServerSettings serverTlsSettings;
    QVERIFY2(TestSupport::loadTestServerTlsSettings(&serverTlsSettings, &probeError),
             qPrintable(probeError));

    TlsConfiguration::ClientSettings clientTlsSettings;
    QVERIFY2(TestSupport::loadTestClientTlsSettings(&clientTlsSettings, &probeError),
             qPrintable(probeError));
}

void TlsIntegrationTests::networkClientCanAuthenticateResumeAndExchangeMessagesOverTls()
{
    QString tlsError;
    TlsConfiguration::ServerSettings serverTlsSettings;
    QVERIFY2(TestSupport::loadTestServerTlsSettings(&serverTlsSettings, &tlsError),
             qPrintable(tlsError));

    TlsConfiguration::ClientSettings clientTlsSettings;
    QVERIFY2(TestSupport::loadTestClientTlsSettings(&clientTlsSettings, &tlsError),
             qPrintable(tlsError));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString databasePath = tempDir.filePath(QStringLiteral("tls_integration.db"));
    std::unique_ptr<Server> server = startServerWithRetry(databasePath, serverTlsSettings);
    QVERIFY(server);

    NetworkClient alice;
    NetworkClient bob;
    alice.setTlsConfiguration(clientTlsSettings);
    bob.setTlsConfiguration(clientTlsSettings);

    QSignalSpy aliceConnectedSpy(&alice, &NetworkClient::socketConnected);
    QSignalSpy bobConnectedSpy(&bob, &NetworkClient::socketConnected);
    QSignalSpy aliceAuthSpy(&alice, &NetworkClient::authSucceeded);
    QSignalSpy bobAuthSpy(&bob, &NetworkClient::authSucceeded);
    QSignalSpy bobDisconnectedSpy(&bob, &NetworkClient::socketDisconnected);
    QSignalSpy bobPrivateSpy(&bob, &NetworkClient::privateMessageReceived);
    QSignalSpy aliceDeliveredSpy(&alice, &NetworkClient::messageDelivered);
    QSignalSpy aliceReadSpy(&alice, &NetworkClient::messageRead);

    alice.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    bob.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    QVERIFY(waitForCount(aliceConnectedSpy, 1));
    QVERIFY(waitForCount(bobConnectedSpy, 1));

    alice.registerUser(QStringLiteral("alice"), QStringLiteral("supersecret"));
    bob.registerUser(QStringLiteral("bob"), QStringLiteral("supersecret"));
    QVERIFY(waitForCount(aliceAuthSpy, 1));
    QVERIFY(waitForCount(bobAuthSpy, 1));

    const QString bobSessionToken = bobAuthSpy.at(0).at(1).toString();
    QVERIFY(!bobSessionToken.isEmpty());

    const QString messageId = alice.sendPrivateMessage(QStringLiteral("bob"), QStringLiteral("secure hello"));
    QVERIFY(waitForCount(bobPrivateSpy, 1));
    QCOMPARE(bobPrivateSpy.at(0).at(0).toString(), messageId);
    QCOMPARE(bobPrivateSpy.at(0).at(1).toString(), QStringLiteral("alice"));
    QCOMPARE(bobPrivateSpy.at(0).at(2).toString(), QStringLiteral("secure hello"));

    QVERIFY(waitForCount(aliceDeliveredSpy, 1));
    QCOMPARE(aliceDeliveredSpy.at(0).at(0).toString(), messageId);

    bob.sendReadReceipt(QStringLiteral("alice"), messageId);
    QVERIFY(waitForCount(aliceReadSpy, 1));
    QCOMPARE(aliceReadSpy.at(0).at(0).toString(), messageId);
    QCOMPARE(aliceReadSpy.at(0).at(1).toString(), QStringLiteral("bob"));

    bob.disconnectFromServer();
    QVERIFY(waitForCount(bobDisconnectedSpy, 1));

    NetworkClient resumedBob;
    resumedBob.setTlsConfiguration(clientTlsSettings);
    QSignalSpy resumedBobConnectedSpy(&resumedBob, &NetworkClient::socketConnected);
    QSignalSpy resumedBobAuthSpy(&resumedBob, &NetworkClient::authSucceeded);

    resumedBob.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    QVERIFY(waitForCount(resumedBobConnectedSpy, 1));

    resumedBob.resumeSession(QStringLiteral("bob"), bobSessionToken);
    QVERIFY(waitForCount(resumedBobAuthSpy, 1));
    QCOMPARE(resumedBobAuthSpy.at(0).at(0).toString(), QStringLiteral("bob"));
    QVERIFY(resumedBobAuthSpy.at(0).at(1).toString() != bobSessionToken);

    alice.disconnectFromServer();
    resumedBob.disconnectFromServer();
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("tls_integration_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    TlsIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tls_integration_tests.moc"
