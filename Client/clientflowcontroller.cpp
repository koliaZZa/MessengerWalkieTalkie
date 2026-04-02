#include "clientflowcontroller.h"

#include "chatwindow.h"
#include "connectionwindow.h"
#include "loginwindow.h"
#include "networkclient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

ClientFlowController::ClientFlowController(QObject* parent)
    : ClientFlowController(QString(), parent)
{
}

ClientFlowController::ClientFlowController(const QString& historyDatabasePath, QObject* parent)
    : QObject(parent)
    , m_client(new NetworkClient(this))
    , m_connectionWindow(new ConnectionWindow())
    , m_loginWindow(new LoginWindow())
    , m_chatWindow(new ChatWindow(&m_historyStore, m_client))
{
    m_historyStore.init(historyDatabasePath);
    connect(m_client, &NetworkClient::socketConnected, this, &ClientFlowController::onSocketConnected);
    connect(m_client, &NetworkClient::socketDisconnected, this, &ClientFlowController::onSocketDisconnected);
    connect(m_client, &NetworkClient::authSucceeded, this, &ClientFlowController::onAuthSucceeded);
    connect(m_client, &NetworkClient::authFailed, this, &ClientFlowController::onAuthFailed);
    connect(m_client, &NetworkClient::sessionInvalid, this, &ClientFlowController::onSessionInvalid);
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

void ClientFlowController::setServerSettingsProvider(ServerSettingsProvider provider)
{
    m_serverSettingsProvider = std::move(provider);
}

void ClientFlowController::setInitialEndpoint(const QString& host, quint16 port)
{
    m_initialHost = host;
    m_initialPort = port;
}

void ClientFlowController::setTlsConfiguration(const TlsConfiguration::ClientSettings& settings)
{
    m_client->setTlsConfiguration(settings);
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
    m_flowState.restoreLastSession(m_historyStore.loadLastSession());
    if (!m_flowState.hasStoredSession()) {
        m_flowState.setEndpoint(m_initialHost, m_initialPort);
    }
    updateEndpointLabels();
    updateOfflineAvailability(false);
    showConnectionWindow();
    attemptConnection(m_flowState.currentHost(), m_flowState.currentPort());
}

void ClientFlowController::onSocketConnected()
{
    updateOfflineAvailability(false);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_flowState.expireStoredSessionIfNeeded(nowMs)) {
        persistStoredSessionState();
    }

    if (m_flowState.canResumeSession(nowMs)) {
        beginSessionResume();
        return;
    }

    if (m_chatWindow->isVisible() && m_flowState.isOfflineViewActive()) {
        m_chatWindow->setStatusText(QStringLiteral("Connected. Sign in to leave offline mode."));
    }

    showLoginWindow(QStringLiteral("Connected to %1:%2")
                        .arg(m_flowState.currentHost())
                        .arg(m_flowState.currentPort()));
}

void ClientFlowController::onSocketDisconnected()
{
    if (m_flowState.handleSocketDisconnected(m_chatWindow->isVisible())) {
        m_chatWindow->activateSession(m_flowState.activeUsername(), true);
        m_chatWindow->setStatusText(QStringLiteral("Offline mode. Reconnecting in background..."));
        return;
    }

    updateOfflineAvailability(m_flowState.hasStoredSession());
    showConnectionWindow();
}

void ClientFlowController::onAuthSucceeded(const QString& username,
                                           const QString& sessionToken,
                                           qint64 sessionExpiresAt)
{
    m_flowState.handleAuthSucceeded(username, sessionToken, sessionExpiresAt);
    persistStoredSessionState();
    m_loginWindow->setBusy(false);
    m_chatWindow->activateSession(username, false);
    m_chatWindow->setStatusText(QStringLiteral("Connected as %1").arg(username));
    showChatWindow();
}

void ClientFlowController::onAuthFailed(const QString& message)
{
    m_flowState.handleAuthFailed();
    m_loginWindow->setBusy(false);
    showLoginWindow(message);
}

void ClientFlowController::onSessionInvalid(const QString& message)
{
    m_flowState.handleSessionInvalid();
    persistStoredSessionState();
    m_loginWindow->setBusy(false);
    showLoginWindow(message.isEmpty()
                        ? QStringLiteral("Session expired. Please sign in again.")
                        : message);
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
        updateOfflineAvailability(m_flowState.hasStoredSession());
    }

    if (m_loginWindow->isVisible()) {
        m_loginWindow->setBusy(false);
        m_loginWindow->setStatusText(message);
    }

    if (m_chatWindow->isVisible() && m_flowState.isOfflineViewActive()) {
        m_chatWindow->setStatusText(QStringLiteral("Offline mode. %1").arg(message));
    }
}

void ClientFlowController::onLoginRequested(const QString& username, const QString& password)
{
    if (username.isEmpty() || password.isEmpty()) {
        m_loginWindow->setStatusText(QStringLiteral("Enter username and password"));
        return;
    }

    m_flowState.markCredentialAuthRequested(username);
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

    m_flowState.markCredentialAuthRequested(username);
    m_loginWindow->setBusy(true);
    m_loginWindow->setStatusText(QStringLiteral("Creating account..."));
    m_client->registerUser(username, password);
}

void ClientFlowController::onOfflineModeRequested()
{
    if (!m_flowState.activateOfflineView()) {
        return;
    }

    m_chatWindow->activateSession(m_flowState.activeUsername(), true);
    m_chatWindow->setStatusText(QStringLiteral("Offline mode. Reconnecting in background..."));
    showChatWindow();
}

void ClientFlowController::onChangeServerRequested()
{
    QString host = m_flowState.currentHost();
    quint16 port = m_flowState.currentPort();
    if (!promptForServerSettings(host, port)) {
        return;
    }

    const bool endpointChanged = host != m_flowState.currentHost() || port != m_flowState.currentPort();
    m_flowState.handleEndpointChange(host, port, endpointChanged);
    updateEndpointLabels();
    showConnectionWindow();
    attemptConnection(m_flowState.currentHost(), m_flowState.currentPort());
}

void ClientFlowController::onLogoutRequested()
{
    const QString username = m_flowState.activeUsername().isEmpty()
                                 ? m_chatWindow->sessionUsername()
                                 : m_flowState.activeUsername();

    m_chatWindow->deactivateSession();

    if (!username.isEmpty()) {
        m_historyStore.clearUserData(username);
    }

    m_client->logout();
    m_client->disconnectFromServer();

    m_flowState.handleLogout();
    persistStoredSessionState();

    m_loginWindow->setUsername(QString());
    m_loginWindow->clearPassword();
    updateOfflineAvailability(false);
    showConnectionWindow();
    attemptConnection(m_flowState.currentHost(), m_flowState.currentPort());
}

void ClientFlowController::attemptConnection(const QString& host, quint16 port)
{
    m_flowState.setEndpoint(host, port);
    updateEndpointLabels();
    updateOfflineAvailability(false);
    m_connectionWindow->setStatusText(QStringLiteral("Connecting to %1:%2")
                                          .arg(m_flowState.currentHost())
                                          .arg(m_flowState.currentPort()));
    m_client->connectToServer(m_flowState.currentHost(), m_flowState.currentPort());
}

void ClientFlowController::beginSessionResume()
{
    if (!m_flowState.canResumeSession(QDateTime::currentMSecsSinceEpoch())) {
        showLoginWindow(QStringLiteral("Connected to %1:%2")
                            .arg(m_flowState.currentHost())
                            .arg(m_flowState.currentPort()));
        return;
    }

    m_flowState.beginSessionResume();
    const LastSessionInfo& lastSession = m_flowState.lastSession();

    const QString status = QStringLiteral("Connected. Restoring session for %1...").arg(lastSession.username);
    m_connectionWindow->setStatusText(status);
    if (m_chatWindow->isVisible() && m_flowState.isOfflineViewActive()) {
        m_chatWindow->setStatusText(status);
    }
    m_client->resumeSession(lastSession.username, lastSession.sessionToken);
}

void ClientFlowController::persistStoredSessionState()
{
    if (!m_flowState.hasStoredSession()) {
        m_historyStore.clearLastSession();
        return;
    }

    m_historyStore.saveLastSession(m_flowState.lastSession());
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
    m_loginWindow->setEndpoint(m_flowState.currentHost(), m_flowState.currentPort());
    m_loginWindow->setUsername(m_flowState.loginPrefillUsername());
    m_loginWindow->clearPassword();
    m_loginWindow->setStatusText(statusText.isEmpty()
                                     ? QStringLiteral("Connected to %1:%2")
                                           .arg(m_flowState.currentHost())
                                           .arg(m_flowState.currentPort())
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
    m_connectionWindow->setEndpoint(m_flowState.currentHost(), m_flowState.currentPort());
    m_loginWindow->setEndpoint(m_flowState.currentHost(), m_flowState.currentPort());
}

void ClientFlowController::updateOfflineAvailability(bool available)
{
    m_connectionWindow->setOfflineModeAvailable(available && m_flowState.hasStoredSession());
}

bool ClientFlowController::promptForServerSettings(QString& host, quint16& port)
{
    if (m_serverSettingsProvider) {
        return m_serverSettingsProvider(host, port);
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Connection settings"));

    auto* layout = new QVBoxLayout(&dialog);
    auto* formLayout = new QFormLayout();
    auto* hostEdit = new QLineEdit(host, &dialog);
    auto* portEdit = new QLineEdit(QString::number(port), &dialog);
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
    const quint16 selectedPort = static_cast<quint16>(portEdit->text().toUShort(&portOk));
    const QString selectedHost = hostEdit->text().trimmed();
    if (!portOk || selectedPort == 0 || selectedHost.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Connection settings"),
                             QStringLiteral("Enter a valid host and port."));
        return false;
    }

    host = selectedHost;
    port = selectedPort;
    return true;
}


