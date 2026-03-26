#pragma once

#include <QObject>

#include "historystore.h"

class ChatWindow;
class ConnectionWindow;
class LoginWindow;
class NetworkClient;

class ClientFlowController : public QObject
{
    Q_OBJECT

public:
    explicit ClientFlowController(QObject* parent = nullptr);
    ~ClientFlowController() override;

    void start();

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onAuthSucceeded(const QString& username);
    void onAuthFailed(const QString& message);
    void onStatusChanged(const QString& message);
    void onTransportError(const QString& message);
    void onLoginRequested(const QString& username, const QString& password);
    void onRegisterRequested(const QString& username, const QString& password);
    void onOfflineModeRequested();
    void onChangeServerRequested();
    void onLogoutRequested();

private:
    void attemptConnection(const QString& host, quint16 port);
    void beginAutoLogin();
    void showConnectionWindow();
    void showLoginWindow(const QString& statusText = QString());
    void showChatWindow();
    void updateEndpointLabels();
    void updateOfflineAvailability(bool available);
    bool hasStoredSession() const;
    bool canAutoLogin() const;
    void persistSuccessfulSession(const QString& username);
    bool promptForServerSettings();

    HistoryStore m_historyStore;
    NetworkClient* m_client;
    ConnectionWindow* m_connectionWindow;
    LoginWindow* m_loginWindow;
    ChatWindow* m_chatWindow;
    LastSessionInfo m_lastSession;
    QString m_currentHost {QStringLiteral("127.0.0.1")};
    quint16 m_currentPort {5555};
    QString m_pendingUsername;
    QString m_pendingPassword;
    bool m_autoLoginPending {false};
    bool m_autoLoginBlocked {false};
    bool m_offlineViewActive {false};
};