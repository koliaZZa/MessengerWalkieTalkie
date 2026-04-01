#include <memory>

#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>

#include "sqlite_test_support.h"
#include "../Client/chatwindow.h"
#include "../Client/historystore.h"
#include "../Client/networkclient.h"
#include "../Server/server.h"
#include "../Shared/authprotocol.h"

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
                                             quint16 port = 0,
                                             int timeoutMs = 5000)
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

QListWidget* dialogsList(ChatWindow& window)
{
    return window.findChild<QListWidget*>(QStringLiteral("chatDialogsList"));
}

QTextEdit* historyView(ChatWindow& window)
{
    return window.findChild<QTextEdit*>(QStringLiteral("chatHistoryView"));
}

QLineEdit* messageEdit(ChatWindow& window)
{
    return window.findChild<QLineEdit*>(QStringLiteral("chatMessageEdit"));
}

bool hasDialog(ChatWindow& window, const QString& dialogName)
{
    QListWidget* list = dialogsList(window);
    return list && !list->findItems(dialogName, Qt::MatchExactly).isEmpty();
}

bool selectDialog(ChatWindow& window, const QString& dialogName)
{
    QListWidget* list = dialogsList(window);
    if (!list) {
        return false;
    }

    const QList<QListWidgetItem*> items = list->findItems(dialogName, Qt::MatchExactly);
    if (items.isEmpty()) {
        return false;
    }

    list->setCurrentItem(items.constFirst());
    QCoreApplication::processEvents();
    return true;
}

}  // namespace

class ChatWindowIntegrationTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void privateDialogReadFlowPersistsOfflineHistory();
    void rejectsOversizedMessageBeforeSending();
};

void ChatWindowIntegrationTests::initTestCase()
{
    QString probeError;
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));
}

void ChatWindowIntegrationTests::privateDialogReadFlowPersistsOfflineHistory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString serverDatabasePath = tempDir.filePath(QStringLiteral("chat_window_server.db"));
    const QString aliceHistoryPath = tempDir.filePath(QStringLiteral("alice_history.db"));
    const QString bobHistoryPath = tempDir.filePath(QStringLiteral("bob_history.db"));

    std::unique_ptr<Server> server = startServerWithRetry(serverDatabasePath);
    QVERIFY(server);

    HistoryStore aliceStore;
    HistoryStore bobStore;
    QVERIFY(aliceStore.init(aliceHistoryPath));
    QVERIFY(bobStore.init(bobHistoryPath));

    NetworkClient aliceClient;
    NetworkClient bobClient;

    QSignalSpy aliceConnectedSpy(&aliceClient, &NetworkClient::socketConnected);
    QSignalSpy bobConnectedSpy(&bobClient, &NetworkClient::socketConnected);
    QSignalSpy aliceAuthSpy(&aliceClient, &NetworkClient::authSucceeded);
    QSignalSpy bobAuthSpy(&bobClient, &NetworkClient::authSucceeded);
    QSignalSpy aliceReadSpy(&aliceClient, &NetworkClient::messageRead);
    QSignalSpy bobReadSpy(&bobClient, &NetworkClient::messageRead);

    aliceClient.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    bobClient.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    QVERIFY(waitForCount(aliceConnectedSpy, 1));
    QVERIFY(waitForCount(bobConnectedSpy, 1));

    aliceClient.registerUser(QStringLiteral("alice"), QStringLiteral("supersecret"));
    bobClient.registerUser(QStringLiteral("bob"), QStringLiteral("supersecret"));
    QVERIFY(waitForCount(aliceAuthSpy, 1));
    QVERIFY(waitForCount(bobAuthSpy, 1));

    ChatWindow aliceWindow(&aliceStore, &aliceClient);
    ChatWindow bobWindow(&bobStore, &bobClient);
    aliceWindow.show();
    bobWindow.show();

    aliceWindow.activateSession(QStringLiteral("alice"), false);
    bobWindow.activateSession(QStringLiteral("bob"), false);

    QTRY_VERIFY_WITH_TIMEOUT(dialogsList(aliceWindow) != nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(dialogsList(bobWindow) != nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(hasDialog(aliceWindow, QStringLiteral("Broadcast")), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(hasDialog(bobWindow, QStringLiteral("Broadcast")), 2000);

    const QString initialMessageId =
        bobClient.sendPrivateMessage(QStringLiteral("alice"), QStringLiteral("hello from bob"));
    QVERIFY(!initialMessageId.isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT(hasDialog(aliceWindow, QStringLiteral("bob")), 5000);
    QVERIFY(bobReadSpy.isEmpty());

    QVERIFY(selectDialog(aliceWindow, QStringLiteral("bob")));
    QTRY_VERIFY_WITH_TIMEOUT(waitForCount(bobReadSpy, 1), 5000);
    QCOMPARE(bobReadSpy.at(0).at(0).toString(), initialMessageId);
    QCOMPARE(bobReadSpy.at(0).at(1).toString(), QStringLiteral("alice"));

    QTRY_VERIFY_WITH_TIMEOUT(historyView(aliceWindow)->toPlainText().contains(QStringLiteral("bob: hello from bob")),
                             5000);
    QVERIFY(historyView(aliceWindow)->toPlainText().contains(QStringLiteral("[read]")));

    QLineEdit* aliceMessageEdit = messageEdit(aliceWindow);
    QVERIFY(aliceMessageEdit);
    aliceMessageEdit->setText(QStringLiteral("reply from alice"));
    QVERIFY(QMetaObject::invokeMethod(&aliceWindow, "sendMessage", Qt::DirectConnection));

    QTRY_VERIFY_WITH_TIMEOUT(hasDialog(bobWindow, QStringLiteral("alice")), 5000);
    QVERIFY(selectDialog(bobWindow, QStringLiteral("alice")));
    QTRY_VERIFY_WITH_TIMEOUT(waitForCount(aliceReadSpy, 1), 5000);
    QCOMPARE(aliceReadSpy.at(0).at(1).toString(), QStringLiteral("bob"));

    QTRY_VERIFY_WITH_TIMEOUT(historyView(bobWindow)->toPlainText().contains(QStringLiteral("alice: reply from alice")),
                             5000);
    QVERIFY(historyView(bobWindow)->toPlainText().contains(QStringLiteral("[read]")));

    aliceClient.disconnectFromServer();
    bobClient.disconnectFromServer();
    QTest::qWait(100);

    NetworkClient offlineClient;
    ChatWindow offlineWindow(&aliceStore, &offlineClient);
    offlineWindow.activateSession(QStringLiteral("alice"), true);

    QTRY_VERIFY_WITH_TIMEOUT(hasDialog(offlineWindow, QStringLiteral("bob")), 2000);
    QVERIFY(selectDialog(offlineWindow, QStringLiteral("bob")));
    QTRY_VERIFY_WITH_TIMEOUT(historyView(offlineWindow)->toPlainText().contains(QStringLiteral("hello from bob")), 2000);
    QVERIFY(historyView(offlineWindow)->toPlainText().contains(QStringLiteral("reply from alice")));

    QLineEdit* offlineMessageEdit = messageEdit(offlineWindow);
    QVERIFY(offlineMessageEdit);
    QVERIFY(!offlineMessageEdit->isEnabled());
    QCOMPARE(offlineMessageEdit->placeholderText(), QStringLiteral("Offline history is read-only"));
}

void ChatWindowIntegrationTests::rejectsOversizedMessageBeforeSending()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString serverDatabasePath = tempDir.filePath(QStringLiteral("chat_window_validation.db"));
    const QString historyDatabasePath = tempDir.filePath(QStringLiteral("validation_history.db"));

    std::unique_ptr<Server> server = startServerWithRetry(serverDatabasePath);
    QVERIFY(server);

    HistoryStore store;
    QVERIFY(store.init(historyDatabasePath));

    NetworkClient client;
    QSignalSpy connectedSpy(&client, &NetworkClient::socketConnected);
    QSignalSpy authSpy(&client, &NetworkClient::authSucceeded);
    QSignalSpy deliveredSpy(&client, &NetworkClient::messageDelivered);

    client.connectToServer(QStringLiteral("127.0.0.1"), server->listeningPort());
    QVERIFY(waitForCount(connectedSpy, 1));

    client.registerUser(QStringLiteral("validator"), QStringLiteral("supersecret"));
    QVERIFY(waitForCount(authSpy, 1));

    ChatWindow window(&store, &client);
    window.show();
    window.activateSession(QStringLiteral("validator"), false);

    QLineEdit* draftEdit = messageEdit(window);
    QTextEdit* history = historyView(window);
    QVERIFY(draftEdit);
    QVERIFY(history);

    draftEdit->setText(QString(AuthProtocol::kMaxMessageLength + 1, QLatin1Char('x')));
    QVERIFY(QMetaObject::invokeMethod(&window, "sendMessage", Qt::DirectConnection));

    QCOMPARE(history->toPlainText(), QString());
    QVERIFY(window.statusText().contains(QStringLiteral("Message must be 1-")));
    QVERIFY(deliveredSpy.isEmpty());

    client.disconnectFromServer();
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chat_window_integration_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    ChatWindowIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "chat_window_integration_tests.moc"
