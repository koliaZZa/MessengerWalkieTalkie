#include <memory>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QTest>

#include "sqlite_test_support.h"
#include "../Client/chatwindow.h"
#include "../Client/clientflowcontroller.h"
#include "../Client/connectionwindow.h"
#include "../Client/loginwindow.h"
#include "../Server/server.h"

namespace {

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

template <typename T>
T* findTopLevelWindow()
{
    const auto widgets = QApplication::topLevelWidgets();
    for (auto widget : widgets) {
        if (auto typed = qobject_cast<T*>(widget)) {
            return typed;
        }
    }

    return nullptr;
}

template <typename T>
T* findVisibleTopLevelWindow()
{
    const auto widgets = QApplication::topLevelWidgets();
    for (auto widget : widgets) {
        if (auto typed = qobject_cast<T*>(widget); typed && typed->isVisible()) {
            return typed;
        }
    }

    return nullptr;
}

bool isLoginAuthErrorVisible()
{
    LoginWindow* loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    if (!loginWindow) {
        return false;
    }

    const QString status = loginWindow->statusText();
    return status.contains(QStringLiteral("User not found"))
           || status.contains(QStringLiteral("Wrong password"));
}

void cleanupController(std::unique_ptr<ClientFlowController>& controller)
{
    controller.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QTest::qWait(50);
}

}

class ClientFlowControllerIntegrationTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registerResumeAndLogoutFlow();
    void offlineModeFlow();
    void changeServerFlow();
};

void ClientFlowControllerIntegrationTests::initTestCase()
{
    QString probeError;
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));
}

void ClientFlowControllerIntegrationTests::registerResumeAndLogoutFlow()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString serverDatabasePath = tempDir.filePath(QStringLiteral("server_auth.db"));
    const QString historyDatabasePath = tempDir.filePath(QStringLiteral("client_history.db"));

    std::unique_ptr<Server> server = startServerWithRetry(serverDatabasePath);
    QVERIFY(server);

    auto controller = std::make_unique<ClientFlowController>(historyDatabasePath);
    controller->setInitialEndpoint(QStringLiteral("127.0.0.1"), server->listeningPort());
    controller->start();

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<LoginWindow>() != nullptr, 5000);
    LoginWindow* loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    QVERIFY(loginWindow);
    QVERIFY(QMetaObject::invokeMethod(loginWindow,
                                      "registerRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("alice")),
                                      Q_ARG(QString, QStringLiteral("supersecret"))));

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ChatWindow>() != nullptr, 5000);
    ChatWindow* chatWindow = findVisibleTopLevelWindow<ChatWindow>();
    QVERIFY(chatWindow);
    QCOMPARE(chatWindow->sessionUsername(), QStringLiteral("alice"));
    QVERIFY(!chatWindow->isOfflineMode());

    cleanupController(controller);

    controller = std::make_unique<ClientFlowController>(historyDatabasePath);
    controller->setInitialEndpoint(QStringLiteral("127.0.0.1"), server->listeningPort());
    controller->start();

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ChatWindow>() != nullptr, 5000);
    chatWindow = findVisibleTopLevelWindow<ChatWindow>();
    QVERIFY(chatWindow);
    QCOMPARE(chatWindow->sessionUsername(), QStringLiteral("alice"));
    QVERIFY(!chatWindow->isOfflineMode());

    QVERIFY(QMetaObject::invokeMethod(chatWindow, "logoutRequested", Qt::DirectConnection));

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<LoginWindow>() != nullptr, 5000);
    loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    QVERIFY(loginWindow);
    QCOMPARE(loginWindow->username(), QString());

    cleanupController(controller);

    controller = std::make_unique<ClientFlowController>(historyDatabasePath);
    controller->setInitialEndpoint(QStringLiteral("127.0.0.1"), server->listeningPort());
    controller->start();

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<LoginWindow>() != nullptr, 5000);
    loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    QVERIFY(loginWindow);
    QCOMPARE(loginWindow->username(), QString());
    QVERIFY(findVisibleTopLevelWindow<ChatWindow>() == nullptr);

    cleanupController(controller);
}

void ClientFlowControllerIntegrationTests::offlineModeFlow()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString serverDatabasePath = tempDir.filePath(QStringLiteral("server_offline.db"));
    const QString historyDatabasePath = tempDir.filePath(QStringLiteral("client_history.db"));

    std::unique_ptr<Server> server = startServerWithRetry(serverDatabasePath);
    QVERIFY(server);

    {
        auto setupController = std::make_unique<ClientFlowController>(historyDatabasePath);
        setupController->setInitialEndpoint(QStringLiteral("127.0.0.1"), server->listeningPort());
        setupController->start();

        QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<LoginWindow>() != nullptr, 5000);
        LoginWindow* loginWindow = findVisibleTopLevelWindow<LoginWindow>();
        QVERIFY(loginWindow);
        QVERIFY(QMetaObject::invokeMethod(loginWindow,
                                          "registerRequested",
                                          Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("offline_user")),
                                          Q_ARG(QString, QStringLiteral("supersecret"))));

        QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ChatWindow>() != nullptr, 5000);
        cleanupController(setupController);
    }

    server.reset();

    auto controller = std::make_unique<ClientFlowController>(historyDatabasePath);
    controller->start();

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ConnectionWindow>() != nullptr, 5000);
    ConnectionWindow* connectionWindow = findVisibleTopLevelWindow<ConnectionWindow>();
    QVERIFY(connectionWindow);
    QTRY_VERIFY_WITH_TIMEOUT(connectionWindow->isOfflineModeAvailable(), 5000);

    QVERIFY(QMetaObject::invokeMethod(connectionWindow, "offlineModeRequested", Qt::DirectConnection));

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ChatWindow>() != nullptr, 5000);
    ChatWindow* chatWindow = findVisibleTopLevelWindow<ChatWindow>();
    QVERIFY(chatWindow);
    QCOMPARE(chatWindow->sessionUsername(), QStringLiteral("offline_user"));
    QVERIFY(chatWindow->isOfflineMode());
    QVERIFY(chatWindow->statusText().contains(QStringLiteral("Offline mode")));

    cleanupController(controller);
}

void ClientFlowControllerIntegrationTests::changeServerFlow()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString serverADatabasePath = tempDir.filePath(QStringLiteral("server_a.db"));
    const QString serverBDatabasePath = tempDir.filePath(QStringLiteral("server_b.db"));
    const QString historyDatabasePath = tempDir.filePath(QStringLiteral("client_history.db"));

    std::unique_ptr<Server> serverA = startServerWithRetry(serverADatabasePath);
    std::unique_ptr<Server> serverB = startServerWithRetry(serverBDatabasePath);
    QVERIFY(serverA);
    QVERIFY(serverB);

    auto controller = std::make_unique<ClientFlowController>(historyDatabasePath);
    controller->setInitialEndpoint(QStringLiteral("127.0.0.1"), serverA->listeningPort());
    controller->setServerSettingsProvider([&](QString& host, quint16& port) {
        host = QStringLiteral("127.0.0.1");
        port = serverB->listeningPort();
        return true;
    });
    controller->start();

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<LoginWindow>() != nullptr, 5000);
    LoginWindow* loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    QVERIFY(loginWindow);
    QVERIFY(QMetaObject::invokeMethod(loginWindow,
                                      "registerRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("alice")),
                                      Q_ARG(QString, QStringLiteral("supersecret"))));

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ChatWindow>() != nullptr, 5000);
    ChatWindow* chatWindow = findVisibleTopLevelWindow<ChatWindow>();
    QVERIFY(chatWindow);
    QCOMPARE(chatWindow->sessionUsername(), QStringLiteral("alice"));

    QVERIFY(QMetaObject::invokeMethod(chatWindow, "changeServerRequested", Qt::DirectConnection));

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<LoginWindow>() != nullptr, 5000);
    loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    QVERIFY(loginWindow);
    QCOMPARE(loginWindow->endpointText(),
             QStringLiteral("Server: 127.0.0.1:%1").arg(serverB->listeningPort()));
    QCOMPARE(loginWindow->username(), QStringLiteral("alice"));
    QVERIFY(findVisibleTopLevelWindow<ChatWindow>() == nullptr);

    QVERIFY(QMetaObject::invokeMethod(loginWindow,
                                      "loginRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("alice")),
                                      Q_ARG(QString, QStringLiteral("supersecret"))));

    QTRY_VERIFY_WITH_TIMEOUT(isLoginAuthErrorVisible(), 5000);
    loginWindow = findVisibleTopLevelWindow<LoginWindow>();
    QVERIFY(loginWindow);
    QVERIFY(loginWindow->statusText().contains(QStringLiteral("User not found"))
            || loginWindow->statusText().contains(QStringLiteral("Wrong password")));

    QVERIFY(QMetaObject::invokeMethod(loginWindow,
                                      "registerRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("bob")),
                                      Q_ARG(QString, QStringLiteral("supersecret"))));

    QTRY_VERIFY_WITH_TIMEOUT(findVisibleTopLevelWindow<ChatWindow>() != nullptr, 5000);
    chatWindow = findVisibleTopLevelWindow<ChatWindow>();
    QVERIFY(chatWindow);
    QCOMPARE(chatWindow->sessionUsername(), QStringLiteral("bob"));
    QVERIFY(!chatWindow->isOfflineMode());

    cleanupController(controller);
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("client_flow_controller_integration_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    ClientFlowControllerIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "client_flow_controller_integration_tests.moc"
