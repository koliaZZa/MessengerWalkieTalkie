#include "chatwindow.h"

#include "networkclient.h"
#include "../Shared/authprotocol.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QVBoxLayout>
#include <algorithm>

ChatWindow::ChatWindow(HistoryStore* historyStore, NetworkClient* client, QWidget* parent)
    : QWidget(parent)
    , m_historyStore(historyStore)
    , m_client(client)
{
    buildUi();

    connect(m_client, &NetworkClient::usersUpdated, this, &ChatWindow::onUsersUpdated);
    connect(m_client, &NetworkClient::dialogsReceived, this, &ChatWindow::onDialogsReceived);
    connect(m_client, &NetworkClient::historyReceived, this, &ChatWindow::onHistoryReceived);
    connect(m_client, &NetworkClient::userLookupFinished, this, &ChatWindow::onUserLookupFinished);
    connect(m_client, &NetworkClient::publicMessageReceived, this, &ChatWindow::onPublicMessage);
    connect(m_client, &NetworkClient::privateMessageReceived, this, &ChatWindow::onPrivateMessage);
    connect(m_client, &NetworkClient::messageQueued, this, [this](const QString& id, const QString& to, qint64 createdAt) {
        ChatMessage message;
        if (m_state.updateMessageStatus(id, QStringLiteral("sending"), createdAt, &message)
            && !m_state.username().isEmpty()) {
            m_historyStore->saveMessage(m_state.username(), message);
        }
        setStatusText(QStringLiteral("User %1 is offline. Message queued until sign in.").arg(to));
        refreshCurrentChat();
    });
    connect(m_client, &NetworkClient::messageDelivered, this, &ChatWindow::onMessageDelivered);
    connect(m_client, &NetworkClient::messageRead, this, &ChatWindow::onMessageRead);
    connect(m_client, &NetworkClient::statusChanged, this, &ChatWindow::onStatusChanged);
    connect(m_client, &NetworkClient::transportError, this, &ChatWindow::onTransportError);

    updateUiState();
}

void ChatWindow::activateSession(const QString& username, bool offlineMode)
{
    const bool userChanged = m_state.username() != username;
    m_state.setUsername(username);
    m_offlineMode = offlineMode;
    m_onlineSession = !offlineMode;

    if (userChanged) {
        clearLoadedHistory();
        loadLocalHistory(username);
    }

    setWindowTitle(QStringLiteral("Messenger Walkie Talkie - %1").arg(username));
    updateUiState();

    if (m_onlineSession && !m_state.username().isEmpty()) {
        m_client->requestDialogList();
        requestCurrentHistory();
    }

    refreshCurrentChat();
}

void ChatWindow::deactivateSession()
{
    m_offlineMode = false;
    m_onlineSession = false;
    m_state.setUsername(QString());
    clearLoadedHistory();
    setWindowTitle(QStringLiteral("Messenger Walkie Talkie"));
    updateUiState();
    setStatusText(QStringLiteral("Disconnected"));
}

void ChatWindow::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

QString ChatWindow::statusText() const
{
    return m_statusLabel->text();
}

QString ChatWindow::sessionUsername() const
{
    return m_state.username();
}

bool ChatWindow::isOfflineMode() const
{
    return m_offlineMode;
}

void ChatWindow::createPrivateDialog()
{
    const QStringList candidates = m_state.onlineUsersForDisplay();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New private dialog"));

    auto* layout = new QVBoxLayout(&dialog);
    auto* hintLabel = new QLabel(QStringLiteral("Choose an online user or enter an existing username."), &dialog);
    hintLabel->setWordWrap(true);

    auto* formLayout = new QFormLayout();
    auto* onlineUsersBox = new QComboBox(&dialog);
    onlineUsersBox->addItem(QString());
    for (auto& candidate : candidates) {
        onlineUsersBox->addItem(candidate);
    }
    onlineUsersBox->setEnabled(!candidates.isEmpty());

    auto* usernameEdit = new QLineEdit(&dialog);
    usernameEdit->setPlaceholderText(QStringLiteral("username"));

    formLayout->addRow(QStringLiteral("Online users"), onlineUsersBox);
    formLayout->addRow(QStringLiteral("Or username"), usernameEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    layout->addWidget(hintLabel);
    layout->addLayout(formLayout);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString manualUsername = usernameEdit->text().trimmed();
    const QString selectedUsername = onlineUsersBox->currentText().trimmed();
    const QString username = !manualUsername.isEmpty() ? manualUsername : selectedUsername;

    if (username.isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("New private dialog"),
                                 QStringLiteral("Choose an online user or enter a username."));
        return;
    }

    if (username == m_state.username()) {
        QMessageBox::warning(this,
                             QStringLiteral("New private dialog"),
                             QStringLiteral("You cannot create a dialog with yourself."));
        return;
    }

    if (!manualUsername.isEmpty()) {
        setStatusText(QStringLiteral("Checking user %1...").arg(username));
        m_client->checkUserExists(username);
        return;
    }

    m_state.ensurePrivateDialog(username);
    rebuildDialogList(username);
    requestCurrentHistory();
    setStatusText(QStringLiteral("Dialog with %1 selected").arg(username));
}

void ChatWindow::sendMessage()
{
    if (!m_onlineSession || m_offlineMode || m_state.username().isEmpty()) {
        return;
    }

    const QString text = m_messageEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }

    if (!AuthProtocol::isMessageTextValid(text)) {
        setStatusText(QStringLiteral("Message must be 1-%1 characters").arg(AuthProtocol::kMaxMessageLength));
        return;
    }

    ChatMessage message;
    message.author = m_state.username();
    message.text = text;
    message.status = QStringLiteral("sending");
    message.chatKey = currentChatKey();
    message.outgoing = true;
    message.createdAt = QDateTime::currentMSecsSinceEpoch();

    if (message.chatKey == QStringLiteral("Broadcast")) {
        message.id = m_client->sendBroadcastMessage(text);
    } else {
        message.id = m_client->sendPrivateMessage(message.chatKey, text);
    }

    appendMessage(message);
    m_messageEdit->clear();
}

void ChatWindow::onUsersUpdated(const QStringList& users)
{
    m_state.setOnlineUsers(users);
    rebuildOnlineUsersList();
}

void ChatWindow::onDialogsReceived(const QStringList& dialogs)
{
    const QString selectedChat = currentChatKey();
    m_state.mergeDialogs(dialogs);
    rebuildDialogList(selectedChat);
}

void ChatWindow::onHistoryReceived(const QString& chatUser, const QJsonArray& items)
{
    m_state.ensurePrivateDialog(chatUser);

    for (auto& value : items) {
        const QJsonObject object = value.toObject();
        ChatMessage message;
        message.id = object.value("id").toString();
        message.chatKey = chatUser;
        message.author = object.value("from").toString();
        message.text = object.value("text").toString();
        message.status = object.value("status").toString(QStringLiteral("delivered"));
        message.outgoing = message.author == m_state.username();
        message.createdAt = object.value("created_at").toVariant().toLongLong();
        appendMessage(message);
    }

    rebuildDialogList(currentChatKey());
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

    m_state.ensurePrivateDialog(username);
    rebuildDialogList(username);
    requestCurrentHistory();
    setStatusText(online ? QStringLiteral("Dialog with %1 created").arg(username)
                         : QStringLiteral("User %1 exists, but is offline").arg(username));
}

void ChatWindow::onPublicMessage(const QString& id, const QString& from, const QString& text, qint64 createdAt)
{
    appendMessage({
        id,
        QStringLiteral("Broadcast"),
        from,
        text,
        QStringLiteral("delivered"),
        from == m_state.username(),
        createdAt
    });
}

void ChatWindow::onPrivateMessage(const QString& id, const QString& from, const QString& text, qint64 createdAt)
{
    const QString selectedChat = currentChatKey();
    m_state.ensurePrivateDialog(from);
    appendMessage({
        id,
        from,
        from,
        text,
        QStringLiteral("delivered"),
        false,
        createdAt
    });
    rebuildDialogList(selectedChat.isEmpty() ? from : selectedChat);

    if (currentChatKey() == from) {
        markCurrentPrivateMessagesRead();
        refreshCurrentChat();
    }
}

void ChatWindow::onMessageDelivered(const QString& id, qint64 createdAt)
{
    ChatMessage message;
    if (!m_state.updateMessageStatus(id, QStringLiteral("delivered"), createdAt, &message)) {
        return;
    }

    if (!m_state.username().isEmpty()) {
        m_historyStore->saveMessage(m_state.username(), message);
    }
    refreshCurrentChat();
}

void ChatWindow::onMessageRead(const QString& id, const QString&)
{
    ChatMessage message;
    if (!m_state.updateMessageStatus(id, QStringLiteral("read"), 0, &message)) {
        return;
    }

    if (!m_state.username().isEmpty()) {
        m_historyStore->saveMessage(m_state.username(), message);
    }
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

void ChatWindow::refreshCurrentChat()
{
    const QString chatKey = currentChatKey();
    QStringList lines;
    const QDate today = QDate::currentDate();

    const QList<ChatMessage> messages = m_state.messagesForChat(chatKey);
    for (auto message : messages) {
        const QDateTime timestamp = QDateTime::fromMSecsSinceEpoch(message.createdAt);
        const QString timeText = timestamp.date() == today
                                     ? timestamp.toString(QStringLiteral("HH:mm:ss"))
                                     : timestamp.toString(QStringLiteral("dd.MM HH:mm"));
        lines.append(QStringLiteral("[%1] [%2] %3: %4")
                         .arg(timeText, message.status, message.author, message.text));
    }

    m_historyView->setPlainText(lines.join('\n'));
}

void ChatWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Messenger Walkie Talkie"));
    resize(980, 620);

    auto* rootLayout = new QVBoxLayout(this);

    auto* headerLayout = new QHBoxLayout();
    m_statusLabel = new QLabel(QStringLiteral("Ready"), this);
    m_statusLabel->setObjectName(QStringLiteral("chatStatusLabel"));
    m_changeServerButton = new QPushButton(QStringLiteral("Change server"), this);
    m_changeServerButton->setObjectName(QStringLiteral("chatChangeServerButton"));
    m_logoutButton = new QPushButton(QStringLiteral("Logout"), this);
    m_logoutButton->setObjectName(QStringLiteral("chatLogoutButton"));
    headerLayout->addWidget(m_statusLabel, 1);
    headerLayout->addWidget(m_changeServerButton);
    headerLayout->addWidget(m_logoutButton);
    rootLayout->addLayout(headerLayout);

    auto* contentLayout = new QHBoxLayout();
    auto* sidebarLayout = new QVBoxLayout();
    auto* dialogsLabel = new QLabel(QStringLiteral("Dialogs"), this);
    m_dialogsList = new QListWidget(this);
    m_dialogsList->setObjectName(QStringLiteral("chatDialogsList"));
    m_newDialogButton = new QPushButton(QStringLiteral("New dialog"), this);
    m_newDialogButton->setObjectName(QStringLiteral("chatNewDialogButton"));
    auto* onlineUsersLabel = new QLabel(QStringLiteral("Online users"), this);
    m_onlineUsersList = new QListWidget(this);
    m_onlineUsersList->setObjectName(QStringLiteral("chatOnlineUsersList"));
    m_onlineUsersList->setSelectionMode(QAbstractItemView::NoSelection);

    sidebarLayout->addWidget(dialogsLabel);
    sidebarLayout->addWidget(m_dialogsList, 3);
    sidebarLayout->addWidget(m_newDialogButton);
    sidebarLayout->addSpacing(8);
    sidebarLayout->addWidget(onlineUsersLabel);
    sidebarLayout->addWidget(m_onlineUsersList, 2);

    m_historyView = new QTextEdit(this);
    m_historyView->setObjectName(QStringLiteral("chatHistoryView"));
    m_historyView->setReadOnly(true);
    contentLayout->addLayout(sidebarLayout, 1);
    contentLayout->addWidget(m_historyView, 3);
    rootLayout->addLayout(contentLayout, 1);

    auto* composerLayout = new QHBoxLayout();
    m_messageEdit = new QLineEdit(this);
    m_messageEdit->setObjectName(QStringLiteral("chatMessageEdit"));
    m_sendButton = new QPushButton(QStringLiteral("Send"), this);
    m_sendButton->setObjectName(QStringLiteral("chatSendButton"));
    composerLayout->addWidget(m_messageEdit, 1);
    composerLayout->addWidget(m_sendButton);
    rootLayout->addLayout(composerLayout);

    connect(m_changeServerButton, &QPushButton::clicked, this, &ChatWindow::changeServerRequested);
    connect(m_logoutButton, &QPushButton::clicked, this, &ChatWindow::logoutRequested);
    connect(m_newDialogButton, &QPushButton::clicked, this, &ChatWindow::createPrivateDialog);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(m_dialogsList, &QListWidget::currentRowChanged, this, [this](int) {
        markCurrentPrivateMessagesRead();
        refreshCurrentChat();
        requestCurrentHistory();
    });
}

void ChatWindow::loadLocalHistory(const QString& username)
{
    if (username.isEmpty()) {
        return;
    }

    const QStringList dialogs = m_historyStore->loadDialogs(username);
    m_state.mergeDialogs(dialogs);

    const QList<ChatMessage> broadcastMessages = m_historyStore->loadMessages(username, QStringLiteral("Broadcast"));
    for (auto message : broadcastMessages) {
        appendMessage(message, false);
    }

    for (auto dialog : dialogs) {
        const QList<ChatMessage> messages = m_historyStore->loadMessages(username, dialog);
        for (auto message : messages) {
            appendMessage(message, false);
        }
    }

    rebuildDialogList(QStringLiteral("Broadcast"));
    rebuildOnlineUsersList();
}

void ChatWindow::appendMessage(const ChatMessage& sourceMessage, bool persist)
{
    const ChatMessage message = m_state.upsertMessage(sourceMessage);

    if (persist && !m_state.username().isEmpty()) {
        m_historyStore->saveMessage(m_state.username(), message);
    }

    refreshCurrentChat();
}

void ChatWindow::clearLoadedHistory()
{
    m_state.clear();
    m_dialogsList->clear();
    m_onlineUsersList->clear();
    m_historyView->clear();
    m_messageEdit->clear();
}

QString ChatWindow::currentChatKey() const
{
    if (!m_dialogsList->currentItem()) {
        return QStringLiteral("Broadcast");
    }

    return m_dialogsList->currentItem()->text();
}

void ChatWindow::markCurrentPrivateMessagesRead()
{
    if (!m_onlineSession || m_offlineMode) {
        return;
    }

    const QString chatKey = currentChatKey();
    if (chatKey == QStringLiteral("Broadcast")) {
        return;
    }

    const QList<ChatMessage> updatedMessages = m_state.markIncomingMessagesRead(chatKey);
    for (auto message : updatedMessages) {
        if (!m_state.username().isEmpty()) {
            m_historyStore->saveMessage(m_state.username(), message);
        }
        m_client->sendReadReceipt(chatKey, message.id);
    }
}

void ChatWindow::rebuildDialogList(const QString& preferredChat)
{
    const QString selectedChat = preferredChat.isEmpty() ? currentChatKey() : preferredChat;
    const QStringList dialogs = m_state.dialogList();

    {
        QSignalBlocker blocker(m_dialogsList);
        m_dialogsList->clear();
        for (auto chat : dialogs) {
            m_dialogsList->addItem(chat);
        }

        QList<QListWidgetItem*> found = m_dialogsList->findItems(selectedChat, Qt::MatchExactly);
        if (!found.isEmpty()) {
            m_dialogsList->setCurrentItem(found.first());
        } else {
            m_dialogsList->setCurrentRow(0);
        }
    }

    refreshCurrentChat();
}

void ChatWindow::rebuildOnlineUsersList()
{
    const QStringList onlineUsers = m_state.onlineUsersForDisplay();

    QSignalBlocker blocker(m_onlineUsersList);
    m_onlineUsersList->clear();
    for (auto username : onlineUsers) {
        m_onlineUsersList->addItem(username);
    }
}

void ChatWindow::requestCurrentHistory()
{
    if (!m_onlineSession || m_offlineMode) {
        return;
    }

    const QString chatKey = currentChatKey();
    if (chatKey.isEmpty()) {
        return;
    }

    m_client->requestHistory(chatKey);
}

void ChatWindow::updateUiState()
{
    const bool canCompose = m_onlineSession && !m_offlineMode;
    m_messageEdit->setEnabled(canCompose);
    m_sendButton->setEnabled(canCompose);
    m_newDialogButton->setEnabled(canCompose);
    m_changeServerButton->setEnabled(!m_state.username().isEmpty());
    m_logoutButton->setEnabled(!m_state.username().isEmpty());
    m_onlineUsersList->setEnabled(canCompose);

    if (m_offlineMode) {
        m_messageEdit->setPlaceholderText(QStringLiteral("Offline history is read-only"));
    } else {
        m_messageEdit->setPlaceholderText(QStringLiteral("Type a message"));
    }
}





