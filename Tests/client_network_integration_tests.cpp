#include <memory>

#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include "sqlite_test_support.h"
#include "../Client/networkclient.h"
#include "../Server/server.h"
#include "../Shared/transportprotocol.h"

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

QByteArray invalidLengthPacketBytes()
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << static_cast<quint32>(Protocol::kMaxPayloadSize + 1);
    bytes.append(QByteArray(Protocol::kFooterSize, '\0'));
    return bytes;
}

std::unique_ptr<Server> startServerWithRetry(const QString& databasePath,
                                             quint16 port = 0,
                                             int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        auto server = std::make_unique<Server>();
        server->setDatabasePath(databasePath);
        if (server->start(port)) {
            return server;
        }

        server.reset();
        QTest::qWait(50);
    }

    return {};
}

}

class ClientNetworkIntegrationTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void queuedPrivateHistoryAndReadFlow();
    void reconnectAndResumeAfterServerRestart();
    void invalidPacketDisconnectsClient();
};

void ClientNetworkIntegrationTests::initTestCase()
{
    QString probeError;
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));
}

void ClientNetworkIntegrationTests::queuedPrivateHistoryAndReadFlow()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString databasePath = tempDir.filePath(QStringLiteral("client_network_queue.db"));
    std::unique_ptr<Server> server = startServerWithRetry(databasePath);
    QVERIFY(server);
    QVERIFY(server->listeningPort() > 0);

    NetworkClient alice;
    NetworkClient bob;

    QSignalSpy aliceConnectedSpy(&alice, &NetworkClient::socketConnected);
    QSignalSpy bobConnectedSpy(&bob, &NetworkClient::socketConnected);
    QSignalSpy aliceAuthSpy(&alice, &NetworkClient::authSucceeded);
    QSignalSpy bobAuthSpy(&bob, &NetworkClient::authSucceeded);
    QSignalSpy bobDisconnectedSpy(&bob, &NetworkClient::socketDisconnected);
    QSignalSpy aliceQueuedSpy(&alice, &NetworkClient::messageQueued);
    QSignalSpy bobPrivateSpy(&bob, &NetworkClient::privateMessageReceived);
    QSignalSpy aliceDeliveredSpy(&alice, &NetworkClient::messageDelivered);
    QSignalSpy aliceReadSpy(&alice, &NetworkClient::messageRead);
    QSignalSpy bobDialogsSpy(&bob, &NetworkClient::dialogsReceived);
    QSignalSpy bobHistorySpy(&bob, &NetworkClient::historyReceived);

    alice.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    bob.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    QVERIFY(waitForCount(aliceConnectedSpy, 1));
    QVERIFY(waitForCount(bobConnectedSpy, 1));

    alice.registerUser(QStringLiteral("alice"), QStringLiteral("supersecret"));
    bob.registerUser(QStringLiteral("bob"), QStringLiteral("supersecret"));
    QVERIFY(waitForCount(aliceAuthSpy, 1));
    QVERIFY(waitForCount(bobAuthSpy, 1));

    bob.disconnectFromServer();
    QVERIFY(waitForCount(bobDisconnectedSpy, 1));

    const QString messageId = alice.sendPrivateMessage(QStringLiteral("bob"), QStringLiteral("queued hello"));
    QVERIFY(waitForCount(aliceQueuedSpy, 1));
    QCOMPARE(aliceQueuedSpy.at(0).at(0).toString(), messageId);
    QCOMPARE(aliceQueuedSpy.at(0).at(1).toString(), QStringLiteral("bob"));

    bob.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    QVERIFY(waitForCount(bobConnectedSpy, 2));
    bob.login(QStringLiteral("bob"), QStringLiteral("supersecret"));
    QVERIFY(waitForCount(bobAuthSpy, 2));
    QVERIFY(waitForCount(bobPrivateSpy, 1));

    QCOMPARE(bobPrivateSpy.at(0).at(0).toString(), messageId);
    QCOMPARE(bobPrivateSpy.at(0).at(1).toString(), QStringLiteral("alice"));
    QCOMPARE(bobPrivateSpy.at(0).at(2).toString(), QStringLiteral("queued hello"));

    QVERIFY(waitForCount(aliceDeliveredSpy, 1));
    QCOMPARE(aliceDeliveredSpy.at(0).at(0).toString(), messageId);

    bob.sendReadReceipt(QStringLiteral("alice"), messageId);
    QVERIFY(waitForCount(aliceReadSpy, 1));
    QCOMPARE(aliceReadSpy.at(0).at(0).toString(), messageId);
    QCOMPARE(aliceReadSpy.at(0).at(1).toString(), QStringLiteral("bob"));

    bobDialogsSpy.clear();
    bob.requestDialogList();
    QVERIFY(waitForCount(bobDialogsSpy, 1));
    QCOMPARE(bobDialogsSpy.at(0).at(0).toStringList(), QStringList({QStringLiteral("alice")}));

    bobHistorySpy.clear();
    bob.requestHistory(QStringLiteral("alice"));
    QVERIFY(waitForCount(bobHistorySpy, 1));
    QCOMPARE(bobHistorySpy.at(0).at(0).toString(), QStringLiteral("alice"));

    const QJsonArray historyItems = bobHistorySpy.at(0).at(1).value<QJsonArray>();
    QCOMPARE(historyItems.size(), 1);
    const QJsonObject message = historyItems.at(0).toObject();
    QCOMPARE(message.value(QStringLiteral("id")).toString(), messageId);
    QCOMPARE(message.value(QStringLiteral("from")).toString(), QStringLiteral("alice"));
    QCOMPARE(message.value(QStringLiteral("to")).toString(), QStringLiteral("bob"));
    QCOMPARE(message.value(QStringLiteral("text")).toString(), QStringLiteral("queued hello"));
    QCOMPARE(message.value(QStringLiteral("status")).toString(), QStringLiteral("read"));

    alice.disconnectFromServer();
    bob.disconnectFromServer();
}

void ClientNetworkIntegrationTests::reconnectAndResumeAfterServerRestart()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString databasePath = tempDir.filePath(QStringLiteral("client_network_reconnect.db"));
    std::unique_ptr<Server> server = startServerWithRetry(databasePath);
    QVERIFY(server);

    const quint16 port = server->listeningPort();
    QVERIFY(port > 0);

    NetworkClient client;
    QSignalSpy connectedSpy(&client, &NetworkClient::socketConnected);
    QSignalSpy disconnectedSpy(&client, &NetworkClient::socketDisconnected);
    QSignalSpy reconnectSpy(&client, &NetworkClient::reconnectScheduled);
    QSignalSpy authSpy(&client, &NetworkClient::authSucceeded);

    client.connectToServer(QStringLiteral("127.0.0.1"), port);
    QVERIFY(waitForCount(connectedSpy, 1));

    client.registerUser(QStringLiteral("reconnect_user"), QStringLiteral("supersecret"));
    QVERIFY(waitForCount(authSpy, 1));
    QCOMPARE(authSpy.at(0).at(0).toString(), QStringLiteral("reconnect_user"));
    const QString sessionToken = authSpy.at(0).at(1).toString();
    QVERIFY(!sessionToken.isEmpty());

    server.reset();

    QVERIFY(waitForCount(disconnectedSpy, 1, 10000));
    QVERIFY(waitForCount(reconnectSpy, 1, 10000));

    server = startServerWithRetry(databasePath, port, 10000);
    QVERIFY(server);
    QCOMPARE(server->listeningPort(), port);

    QVERIFY(waitForCount(connectedSpy, 2, 15000));

    client.resumeSession(QStringLiteral("reconnect_user"), sessionToken);
    QVERIFY(waitForCount(authSpy, 2));
    QCOMPARE(authSpy.at(1).at(0).toString(), QStringLiteral("reconnect_user"));
    QVERIFY(authSpy.at(1).at(1).toString() != sessionToken);

    client.disconnectFromServer();
}

void ClientNetworkIntegrationTests::invalidPacketDisconnectsClient()
{
    QTcpServer badServer;
    QVERIFY(badServer.listen(QHostAddress::LocalHost, 0));

    connect(&badServer, &QTcpServer::newConnection, &badServer, [&badServer]() {
        QTcpSocket* socket = badServer.nextPendingConnection();
        if (!socket) {
            return;
        }

        socket->write(invalidLengthPacketBytes());
        socket->flush();
    });

    NetworkClient client;
    QSignalSpy connectedSpy(&client, &NetworkClient::socketConnected);
    QSignalSpy disconnectedSpy(&client, &NetworkClient::socketDisconnected);
    QSignalSpy errorSpy(&client, &NetworkClient::transportError);

    client.connectToServer(QStringLiteral("127.0.0.1"), badServer.serverPort());
    QVERIFY(waitForCount(connectedSpy, 1));
    QVERIFY(waitForCount(errorSpy, 1));
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("Invalid packet"));
    QVERIFY(waitForCount(disconnectedSpy, 1));

    client.disconnectFromServer();
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("client_network_integration_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    ClientNetworkIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "client_network_integration_tests.moc"
