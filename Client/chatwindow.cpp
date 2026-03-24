#include "chatwindow.h"

#include "networkclient.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>

ChatWindow::ChatWindow(QWidget* parent)
    : QWidget(parent)
    , m_client(new NetworkClient(this))
{
    buildUi();

    connect(m_client, &NetworkClient::authSucceeded, this, &ChatWindow::onAuthSucceeded);
    connect(m_client, &NetworkClient::authFailed, this, &ChatWindow::onAuthFailed);
    connect(m_client, &NetworkClient::usersUpdated, this, &ChatWindow::onUsersUpdated);
    connect(m_client, &NetworkClient::userLookupFinished, this, &ChatWindow::onUserLookupFinished);
    connect(m_client, &NetworkClient::publicMessageReceived, this, &ChatWindow::onPublicMessage);
    connect(m_client, &NetworkClient::privateMessageReceived, this, &ChatWindow::onPrivateMessage);
    connect(m_client, &NetworkClient::messageDelivered, this, &ChatWindow::onMessageDelivered);
    connect(m_client, &NetworkClient::messageRead, this, &ChatWindow::onMessageRead);
    connect(m_client, &NetworkClient::statusChanged, this, &ChatWindow::onStatusChanged);
    connect(m_client, &NetworkClient::transportError, this, &ChatWindow::onTransportError);
    connect(m_client, &NetworkClient::socketDisconnected, this, &ChatWindow::onSocketDisconnected);
}

void ChatWindow::connectToServer()
{
    m_client->connectToServer(m_hostEdit->text(), m_portEdit->text().toUShort());
}

void ChatWindow::login()
{
    m_client->login(m_usernameEdit->text(), m_passwordEdit->text());
}

void ChatWindow::registerUser()
{
    m_client->registerUser(m_usernameEdit->text(), m_passwordEdit->text());
}

void ChatWindow::logout()
{
    m_client->disconnectFromServer();
    clearSession();
    m_pages->setCurrentWidget(m_loginPage);
}

void ChatWindow::createPrivateDialog()
{
    bool accepted = false;
    const QString username = QInputDialog::getText(this,
                                                   QStringLiteral("New private dialog"),
                                                   QStringLiteral("Username"),
                                                   QLineEdit::Normal,
                                                   QString(),
                                                   &accepted)
                                 .trimmed();

    if (!accepted || username.isEmpty()) {
        return;
    }

    if (username == m_username) {
        QMessageBox::information(this,
                                 QStringLiteral("Private dialog"),
                                 QStringLiteral("You cannot create a dialog with yourself."));
        return;
    }

    if (m_privateDialogs.contains(username) || m_onlineUsers.contains(username)) {
        ensurePrivateDialog(username);
        rebuildUsersList(username);
        return;
    }

    setStatusText(QStringLiteral("Checking user %1").arg(username));
    m_client->checkUserExists(username);
}

void ChatWindow::sendMessage()
{
    const QString text = m_messageEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }

    ChatMessage message;
    message.author = m_username;
    message.text = text;
    message.status = QStringLiteral("sending");
    message.chatKey = currentChatKey();
    message.outgoing = true;

    if (message.chatKey == QStringLiteral("Broadcast")) {
        message.id = m_client->sendBroadcastMessage(text);
    } else {
        message.id = m_client->sendPrivateMessage(message.chatKey, text);
    }

    appendMessage(message);
    m_messageEdit->clear();
}

void ChatWindow::onAuthSucceeded(const QString& username)
{
    m_username = username;
    m_onlineUsers.clear();
    m_privateDialogs.clear();
    m_pages->setCurrentWidget(m_chatPage);
    rebuildUsersList(QStringLiteral("Broadcast"));
    setStatusText(QStringLiteral("Logged in as %1").arg(username));
}

void ChatWindow::onAuthFailed(const QString& message)
{
    QMessageBox::warning(this, QStringLiteral("Authentication failed"), message);
}

void ChatWindow::onUsersUpdated(const QStringList& users)
{
    m_onlineUsers = users;
    rebuildUsersList();
}

void ChatWindow::onUserLookupFinished(const QString& username, bool exists, bool online)
{
    if (!exists) {
        QMessageBox::warning(this,
                             QStringLiteral("Private dialog"),
                             QStringLiteral("User %1 does not exist.").arg(username));
        setStatusText(QStringLiteral("User %1 not found").arg(username));
        return;
    }

    ensurePrivateDialog(username);
    rebuildUsersList(username);
    setStatusText(online ? QStringLiteral("Dialog with %1 created").arg(username)
                         : QStringLiteral("User %1 exists, but is offline").arg(username));
}

void ChatWindow::onPublicMessage(const QString& id, const QString& from, const QString& text)
{
    appendMessage({
        id,
        QStringLiteral("Broadcast"),
        from,
        text,
        QStringLiteral("delivered"),
        false
    });
}

void ChatWindow::onPrivateMessage(const QString& id, const QString& from, const QString& text)
{
    const QString selectedChat = currentChatKey();
    ensurePrivateDialog(from);
    appendMessage({
        id,
        from,
        from,
        text,
        QStringLiteral("delivered"),
        false
    });
    rebuildUsersList(selectedChat);

    if (currentChatKey() == from) {
        markCurrentPrivateMessagesRead();
        refreshCurrentChat();
    }
}

void ChatWindow::onMessageDelivered(const QString& id)
{
    if (!m_messagesById.contains(id)) {
        return;
    }

    m_messagesById[id].status = QStringLiteral("delivered");
    refreshCurrentChat();
}

void ChatWindow::onMessageRead(const QString& id, const QString&)
{
    if (!m_messagesById.contains(id)) {
        return;
    }

    m_messagesById[id].status = QStringLiteral("read");
    refreshCurrentChat();
}

void ChatWindow::onStatusChanged(const QString& message)
{
    setStatusText(message);
}

void ChatWindow::onTransportError(const QString& message)
{
    setStatusText(message);
}

void ChatWindow::onSocketDisconnected()
{
    clearSession();
    m_pages->setCurrentWidget(m_loginPage);
}

void ChatWindow::refreshCurrentChat()
{
    const QString chatKey = currentChatKey();
    QStringList lines;

    const QStringList ids = m_chatMessages.value(chatKey);
    for (const QString& id : ids) {
        const ChatMessage& message = m_messagesById[id];
        lines.append(QStringLiteral("[%1] %2: %3")
                         .arg(message.status, message.author, message.text));
    }

    m_historyView->setPlainText(lines.join('\n'));
}

void ChatWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Messenger"));
    resize(980, 620);

    auto* rootLayout = new QVBoxLayout(this);
    m_pages = new QStackedWidget(this);
    rootLayout->addWidget(m_pages);

    m_loginPage = new QWidget(this);
    auto* loginLayout = new QVBoxLayout(m_loginPage);
    m_loginStatusLabel = new QLabel(QStringLiteral("Disconnected"), m_loginPage);
    m_hostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), m_loginPage);
    m_portEdit = new QLineEdit(QStringLiteral("5555"), m_loginPage);
    m_usernameEdit = new QLineEdit(m_loginPage);
    m_passwordEdit = new QLineEdit(m_loginPage);
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    auto* connectButton = new QPushButton(QStringLiteral("Connect"), m_loginPage);
    auto* loginButton = new QPushButton(QStringLiteral("Login"), m_loginPage);
    auto* registerButton = new QPushButton(QStringLiteral("Register"), m_loginPage);

    loginLayout->addWidget(new QLabel(QStringLiteral("Host"), m_loginPage));
    loginLayout->addWidget(m_hostEdit);
    loginLayout->addWidget(new QLabel(QStringLiteral("Port"), m_loginPage));
    loginLayout->addWidget(m_portEdit);
    loginLayout->addWidget(connectButton);
    loginLayout->addSpacing(12);
    loginLayout->addWidget(new QLabel(QStringLiteral("Username"), m_loginPage));
    loginLayout->addWidget(m_usernameEdit);
    loginLayout->addWidget(new QLabel(QStringLiteral("Password"), m_loginPage));
    loginLayout->addWidget(m_passwordEdit);
    loginLayout->addWidget(loginButton);
    loginLayout->addWidget(registerButton);
    loginLayout->addWidget(m_loginStatusLabel);
    loginLayout->addStretch();

    m_chatPage = new QWidget(this);
    auto* chatLayout = new QVBoxLayout(m_chatPage);
    auto* headerLayout = new QHBoxLayout();
    m_chatStatusLabel = new QLabel(QStringLiteral("Ready"), m_chatPage);
    m_logoutButton = new QPushButton(QStringLiteral("Logout"), m_chatPage);
    headerLayout->addWidget(m_chatStatusLabel, 1);
    headerLayout->addWidget(m_logoutButton);
    chatLayout->addLayout(headerLayout);

    auto* contentLayout = new QHBoxLayout();
    auto* usersLayout = new QVBoxLayout();
    m_usersList = new QListWidget(m_chatPage);
    m_newDialogButton = new QPushButton(QStringLiteral("New dialog"), m_chatPage);
    usersLayout->addWidget(m_usersList, 1);
    usersLayout->addWidget(m_newDialogButton);

    m_historyView = new QTextEdit(m_chatPage);
    m_historyView->setReadOnly(true);
    contentLayout->addLayout(usersLayout, 1);
    contentLayout->addWidget(m_historyView, 3);
    chatLayout->addLayout(contentLayout, 1);

    auto* composerLayout = new QHBoxLayout();
    m_messageEdit = new QLineEdit(m_chatPage);
    m_sendButton = new QPushButton(QStringLiteral("Send"), m_chatPage);
    composerLayout->addWidget(m_messageEdit, 1);
    composerLayout->addWidget(m_sendButton);
    chatLayout->addLayout(composerLayout);

    m_pages->addWidget(m_loginPage);
    m_pages->addWidget(m_chatPage);
    m_pages->setCurrentWidget(m_loginPage);

    connect(connectButton, &QPushButton::clicked, this, &ChatWindow::connectToServer);
    connect(loginButton, &QPushButton::clicked, this, &ChatWindow::login);
    connect(registerButton, &QPushButton::clicked, this, &ChatWindow::registerUser);
    connect(m_logoutButton, &QPushButton::clicked, this, &ChatWindow::logout);
    connect(m_newDialogButton, &QPushButton::clicked, this, &ChatWindow::createPrivateDialog);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(m_usersList, &QListWidget::currentRowChanged, this, [this](int) {
        markCurrentPrivateMessagesRead();
        refreshCurrentChat();
    });
}

void ChatWindow::appendMessage(const ChatMessage& message)
{
    m_messagesById.insert(message.id, message);
    m_chatMessages[message.chatKey].append(message.id);
    refreshCurrentChat();
}

void ChatWindow::clearSession()
{
    m_username.clear();
    m_onlineUsers.clear();
    m_privateDialogs.clear();
    m_messagesById.clear();
    m_chatMessages.clear();
    m_usersList->clear();
    m_historyView->clear();
    m_messageEdit->clear();
    setStatusText(QStringLiteral("Disconnected"));
}

QString ChatWindow::currentChatKey() const
{
    if (!m_usersList->currentItem()) {
        return QStringLiteral("Broadcast");
    }

    return m_usersList->currentItem()->text();
}

void ChatWindow::ensurePrivateDialog(const QString& username)
{
    if (username.isEmpty() || username == m_username || username == QStringLiteral("Broadcast")) {
        return;
    }

    if (!m_privateDialogs.contains(username)) {
        m_privateDialogs.append(username);
    }
}

void ChatWindow::markCurrentPrivateMessagesRead()
{
    const QString chatKey = currentChatKey();
    if (chatKey == QStringLiteral("Broadcast")) {
        return;
    }

    const QStringList ids = m_chatMessages.value(chatKey);
    for (const QString& id : ids) {
        ChatMessage& message = m_messagesById[id];
        if (!message.outgoing && message.status != QStringLiteral("read")) {
            message.status = QStringLiteral("read");
            m_client->sendReadReceipt(chatKey, id);
        }
    }
}

void ChatWindow::rebuildUsersList(const QString& preferredChat)
{
    const QString selectedChat = preferredChat.isEmpty() ? currentChatKey() : preferredChat;
    QStringList chats = m_privateDialogs;

    for (const QString& username : m_onlineUsers) {
        if (username != m_username && !chats.contains(username)) {
            chats.append(username);
        }
    }

    chats.sort(Qt::CaseInsensitive);

    {
        QSignalBlocker blocker(m_usersList);
        m_usersList->clear();
        m_usersList->addItem(QStringLiteral("Broadcast"));
        for (const QString& chat : chats) {
            m_usersList->addItem(chat);
        }

        QList<QListWidgetItem*> found = m_usersList->findItems(selectedChat, Qt::MatchExactly);
        if (!found.isEmpty()) {
            m_usersList->setCurrentItem(found.first());
        } else {
            m_usersList->setCurrentRow(0);
        }
    }

    markCurrentPrivateMessagesRead();
    refreshCurrentChat();
}

void ChatWindow::setStatusText(const QString& text)
{
    m_loginStatusLabel->setText(text);
    m_chatStatusLabel->setText(text);
}
