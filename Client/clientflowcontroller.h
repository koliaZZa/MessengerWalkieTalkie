#pragma once

#include <functional>

#include <QObject>

#include "clientflowstate.h"
#include "historystore.h"

class ChatWindow;
class ConnectionWindow;
class LoginWindow;
class NetworkClient;

class ClientFlowController : public QObject
{
    Q_OBJECT

public:
    using ServerSettingsProvider = std::function<bool(QString&, quint16&)>;

    explicit ClientFlowController(QObject* parent);
    explicit ClientFlowController(const QString& historyDatabasePath = QString(),
                                  QObject* parent = nullptr);
    ~ClientFlowController() override;

    void setInitialEndpoint(const QString& host, quint16 port);
    void start();
    void setServerSettingsProvider(ServerSettingsProvider provider);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onAuthSucceeded(const QString& username, const QString& sessionToken, qint64 sessionExpiresAt);
    void onAuthFailed(const QString& message);
    void onSessionInvalid(const QString& message);
    void onStatusChanged(const QString& message);
    void onTransportError(const QString& message);
    void onLoginRequested(const QString& username, const QString& password);
    void onRegisterRequested(const QString& username, const QString& password);
    void onOfflineModeRequested();
    void onChangeServerRequested();
    void onLogoutRequested();

private:
    void attemptConnection(const QString& host, quint16 port);
    void beginSessionResume();
    void persistStoredSessionState();
    void showConnectionWindow();
    void showLoginWindow(const QString& statusText = QString());
    void showChatWindow();
    void updateEndpointLabels();
    void updateOfflineAvailability(bool available);
    bool promptForServerSettings(QString& host, quint16& port);

    HistoryStore m_historyStore;
    NetworkClient* m_client;
    ConnectionWindow* m_connectionWindow;
    LoginWindow* m_loginWindow;
    ChatWindow* m_chatWindow;
    ClientFlowState m_flowState;
    ServerSettingsProvider m_serverSettingsProvider;
    QString m_initialHost {QStringLiteral("127.0.0.1")};
    quint16 m_initialPort {5555};
};

