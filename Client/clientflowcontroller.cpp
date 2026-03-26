#include "clientflowcontroller.h"

#include "chatwindow.h"
#include "connectionwindow.h"
#include "loginwindow.h"
#include "networkclient.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

ClientFlowController::ClientFlowController(QObject* parent)
    : QObject(parent)
    , m_client(new NetworkClient(this))
    , m_connectionWindow(new ConnectionWindow())
    , m_loginWindow(new LoginWindow())
    , m_chatWindow(new ChatWindow(&m_historyStore, m_client))
{
    m_historyStore.init();
    connect(m_client, &NetworkClient::socketConnected, this, &ClientFlowController::onSocketConnected);
    connect(m_client, &NetworkClient::socketDisconnected, this, &ClientFlowController::onSocketDisconnected);
    connect(m_client, &NetworkClient::authSucceeded, this, &ClientFlowController::onAuthSucceeded);
    connect(m_client, &NetworkClient::authFailed, this, &ClientFlowController::onAuthFailed);
    connect(m_client, &NetworkClient::statusChanged, this, &ClientFlowController::onStatusChanged);
    connect(m_client, &NetworkClient::transportError, this, &ClientFlowController::onTransportError);

    connect(m_connectionWindow, &ConnectionWindow::settingsRequested, this, &ClientFlowController::onChangeServerRequested);
    connect(m_connectionWindow, &ConnectionWindow::offlineModeRequested, this, &ClientFlowController::onOfflineModeRequested);
    connect(m_loginWindow, &LoginWindow::loginRequested, this, &ClientFlowController::onLoginRequested);
    connect(m_loginWindow, &LoginWindow::registerRequested, this, &ClientFlowController::onRegisterRequested);
    connect(m_loginWindow, &LoginWindow::changeServerRequested, this, &ClientFlowController::onChangeServerRequested);
    connect(m_chatWindow, &ChatWindow::changeServerRequested, this, &ClientFlowController::onChangeServerRequested);
    connect(m_chatWindow, &ChatWindow::logoutRequested, this, &ClientFlowController::onLogoutRequested);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_client->disconnectFromServer();
    });
}

ClientFlowController::~ClientFlowController()
{
    m_client->disconnectFromServer();
    delete m_chatWindow;
    delete m_loginWindow;
    delete m_connectionWindow;
}

void ClientFlowController::start()
{
    m_lastSession = m_historyStore.loadLastSession();
    if (m_lastSession.hasStoredIdentity()) {
        m_currentHost = m_lastSession.host;
        m_currentPort = m_lastSession.port;
    }

    updateEndpointLabels();
    updateOfflineAvailability(false);
    showConnectionWindow();
    attemptConnection(m_currentHost, m_currentPort);
}

void ClientFlowController::onSocketConnected()
{
    updateOfflineAvailability(false);

    if (canAutoLogin()) {
        beginAutoLogin();
        return;
    }

    if (m_chatWindow->isVisible() && m_offlineViewActive) {
        m_chatWindow->setStatusText(QStringLiteral("Connected. Sign in to leave offline mode."));
    }

    showLoginWindow(QStringLiteral("Connected to %1:%2").arg(m_currentHost).arg(m_currentPort));
}

void ClientFlowController::onSocketDisconnected()
{
    if (m_chatWindow->isVisible() && !m_chatWindow->sessionUsername().isEmpty()) {
        m_offlineViewActive = true;
        m_chatWindow->activateSession(m_chatWindow->sessionUsername(), true);
        m_chatWindow->setStatusText(QStringLiteral("Offline mode. Reconnecting in background..."));
        return;
    }

    updateOfflineAvailability(hasStoredSession());
    showConnectionWindow();
}

void ClientFlowController::onAuthSucceeded(const QString& username)
{
    m_autoLoginPending = false;
    m_autoLoginBlocked = false;
    m_offlineViewActive = false;

    persistSuccessfulSession(username);
    m_loginWindow->setBusy(false);
    m_chatWindow->activateSession(username, false);
    m_chatWindow->setStatusText(QStringLiteral("Connected as %1").arg(username));
    showChatWindow();
}

void ClientFlowController::onAuthFailed(const QString& message)
{
    const bool autoLoginFailure = m_autoLoginPending;
    m_autoLoginPending = false;
    m_loginWindow->setBusy(false);

    if (autoLoginFailure) {
        m_autoLoginBlocked = true;
    }

    showLoginWindow(message);
}

void ClientFlowController::onStatusChanged(const QString& message)
{
    m_connectionWindow->setStatusText(message);

    if (m_loginWindow->isVisible()) {
        m_loginWindow->setStatusText(message);
    }

    if (m_chatWindow->isVisible()) {
        m_chatWindow->setStatusText(message);
    }
}

void ClientFlowController::onTransportError(const QString& message)
{
    m_connectionWindow->setStatusText(message);

    if (!m_client->isConnected()) {
        updateOfflineAvailability(hasStoredSession());
    }

    if (m_loginWindow->isVisible()) {
        m_loginWindow->setBusy(false);
        m_loginWindow->setStatusText(message);
    }

    if (m_chatWindow->isVisible() && m_offlineViewActive) {
        m_chatWindow->setStatusText(QStringLiteral("Offline mode. %1").arg(message));
    }
}

void ClientFlowController::onLoginRequested(const QString& username, const QString& password)
{
    if (username.isEmpty() || password.isEmpty()) {
        m_loginWindow->setStatusText(QStringLiteral("Enter username and password"));
        return;
    }

    m_pendingUsername = username;
    m_pendingPassword = password;
    m_autoLoginPending = false;
    m_autoLoginBlocked = false;
    m_loginWindow->setBusy(true);
    m_loginWindow->setStatusText(QStringLiteral("Signing in..."));
    m_client->login(username, password);
}

void ClientFlowController::onRegisterRequested(const QString& username, const QString& password)
{
    if (username.isEmpty() || password.isEmpty()) {
        m_loginWindow->setStatusText(QStringLiteral("Enter username and password"));
        return;
    }

    m_pendingUsername = username;
    m_pendingPassword = password;
    m_autoLoginPending = false;
    m_autoLoginBlocked = false;
    m_loginWindow->setBusy(true);
    m_loginWindow->setStatusText(QStringLiteral("Creating account..."));
    m_client->registerUser(username, password);
}

void ClientFlowController::onOfflineModeRequested()
{
    if (!hasStoredSession()) {
        return;
    }

    m_offlineViewActive = true;
    m_chatWindow->activateSession(m_lastSession.username, true);
    m_chatWindow->setStatusText(QStringLiteral("Offline mode. Reconnecting in background..."));
    showChatWindow();
}

void ClientFlowController::onChangeServerRequested()
{
    if (!promptForServerSettings()) {
        return;
    }

    m_autoLoginBlocked = false;
    m_autoLoginPending = false;
    m_offlineViewActive = false;
    showConnectionWindow();
    attemptConnection(m_currentHost, m_currentPort);
}

void ClientFlowController::onLogoutRequested()
{
    const QString username = m_chatWindow->sessionUsername();
    if (!username.isEmpty()) {
        m_historyStore.clearUserData(username);
    }

    m_historyStore.clearLastSession();
    m_lastSession = LastSessionInfo{};
    m_pendingUsername.clear();
    m_pendingPassword.clear();
    m_autoLoginPending = false;
    m_autoLoginBlocked = false;
    m_offlineViewActive = false;

    m_chatWindow->deactivateSession();
    m_loginWindow->setUsername(QString());
    m_loginWindow->clearPassword();
    updateOfflineAvailability(false);
    showConnectionWindow();
    attemptConnection(m_currentHost, m_currentPort);
}

void ClientFlowController::attemptConnection(const QString& host, quint16 port)
{
    m_currentHost = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    m_currentPort = port == 0 ? 5555 : port;
    updateEndpointLabels();
    updateOfflineAvailability(false);
    m_connectionWindow->setStatusText(QStringLiteral("Connecting to %1:%2").arg(m_currentHost).arg(m_currentPort));
    m_client->connectToServer(m_currentHost, m_currentPort);
}

void ClientFlowController::beginAutoLogin()
{
    if (!canAutoLogin()) {
        showLoginWindow(QStringLiteral("Connected to %1:%2").arg(m_currentHost).arg(m_currentPort));
        return;
    }

    m_autoLoginPending = true;
    m_pendingUsername = m_lastSession.username;
    m_pendingPassword = m_lastSession.password;

    const QString status = QStringLiteral("Connected. Signing in as %1...").arg(m_lastSession.username);
    m_connectionWindow->setStatusText(status);
    if (m_chatWindow->isVisible() && m_offlineViewActive) {
        m_chatWindow->setStatusText(status);
    }
    m_client->login(m_lastSession.username, m_lastSession.password);
}

void ClientFlowController::showConnectionWindow()
{
    m_loginWindow->hide();
    m_chatWindow->hide();
    m_connectionWindow->show();
    m_connectionWindow->raise();
    m_connectionWindow->activateWindow();
}

void ClientFlowController::showLoginWindow(const QString& statusText)
{
    m_connectionWindow->hide();
    m_chatWindow->hide();
    m_loginWindow->setBusy(false);
    m_loginWindow->setEndpoint(m_currentHost, m_currentPort);
    m_loginWindow->setUsername(m_pendingUsername.isEmpty() ? m_lastSession.username : m_pendingUsername);
    if (!m_pendingPassword.isEmpty()) {
        m_loginWindow->setPassword(m_pendingPassword);
    } else if (m_lastSession.hasCredentials()) {
        m_loginWindow->setPassword(m_lastSession.password);
    } else {
        m_loginWindow->clearPassword();
    }
    m_loginWindow->setStatusText(statusText.isEmpty()
                                     ? QStringLiteral("Connected to %1:%2").arg(m_currentHost).arg(m_currentPort)
                                     : statusText);
    m_loginWindow->show();
    m_loginWindow->raise();
    m_loginWindow->activateWindow();
}

void ClientFlowController::showChatWindow()
{
    m_connectionWindow->hide();
    m_loginWindow->hide();
    m_chatWindow->show();
    m_chatWindow->raise();
    m_chatWindow->activateWindow();
}

void ClientFlowController::updateEndpointLabels()
{
    m_connectionWindow->setEndpoint(m_currentHost, m_currentPort);
    m_loginWindow->setEndpoint(m_currentHost, m_currentPort);
}

void ClientFlowController::updateOfflineAvailability(bool available)
{
    m_connectionWindow->setOfflineModeAvailable(available && hasStoredSession());
}

bool ClientFlowController::hasStoredSession() const
{
    return m_lastSession.hasStoredIdentity();
}

bool ClientFlowController::canAutoLogin() const
{
    return m_lastSession.hasCredentials() && !m_autoLoginBlocked;
}

void ClientFlowController::persistSuccessfulSession(const QString& username)
{
    LastSessionInfo session;
    session.username = username;
    session.password = !m_pendingPassword.isEmpty() ? m_pendingPassword : m_lastSession.password;
    session.host = m_currentHost;
    session.port = m_currentPort;

    if (session.hasStoredIdentity()) {
        m_lastSession = session;
        m_historyStore.saveLastSession(session);
    }
}

bool ClientFlowController::promptForServerSettings()
{
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Connection settings"));

    auto* layout = new QVBoxLayout(&dialog);
    auto* formLayout = new QFormLayout();
    auto* hostEdit = new QLineEdit(m_currentHost, &dialog);
    auto* portEdit = new QLineEdit(QString::number(m_currentPort), &dialog);
    formLayout->addRow(QStringLiteral("Host"), hostEdit);
    formLayout->addRow(QStringLiteral("Port"), portEdit);
    layout->addLayout(formLayout);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    bool portOk = false;
    const quint16 port = static_cast<quint16>(portEdit->text().toUShort(&portOk));
    const QString host = hostEdit->text().trimmed();
    if (!portOk || port == 0 || host.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Connection settings"),
                             QStringLiteral("Enter a valid host and port."));
        return false;
    }

    m_currentHost = host;
    m_currentPort = port;
    updateEndpointLabels();
    return true;
}



