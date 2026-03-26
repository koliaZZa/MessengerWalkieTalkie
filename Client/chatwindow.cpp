#include "chatwindow.h"

#include "networkclient.h"

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
        if (m_messagesById.contains(id)) {
            ChatMessage& message = m_messagesById[id];
            message.status = QStringLiteral("sending");
            if (createdAt > 0) {
                message.createdAt = createdAt;
            }
            if (!m_username.isEmpty()) {
                m_historyStore->saveMessage(m_username, message);
            }
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
    const bool userChanged = m_username != username;
    m_username = username;
    m_offlineMode = offlineMode;
    m_onlineSession = !offlineMode;

    if (userChanged) {
        clearLoadedHistory();
        loadLocalHistory(username);
    }

    setWindowTitle(QStringLiteral("Messenger Walkie Talkie - %1").arg(username));
    updateUiState();

    if (m_onlineSession && !m_username.isEmpty()) {
        m_client->requestDialogList();
        requestCurrentHistory();
    }

    refreshCurrentChat();
}

void ChatWindow::deactivateSession()
{
    m_offlineMode = false;
    m_onlineSession = false;
    m_username.clear();
    clearLoadedHistory();
    setWindowTitle(QStringLiteral("Messenger Walkie Talkie"));
    updateUiState();
    setStatusText(QStringLiteral("Disconnected"));
}

void ChatWindow::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

QString ChatWindow::sessionUsername() const
{
    return m_username;
}

bool ChatWindow::isOfflineMode() const
{
    return m_offlineMode;
}

void ChatWindow::createPrivateDialog()
{
    QStringList candidates = m_onlineUsers;
    candidates.removeAll(m_username);
    candidates.removeDuplicates();
    candidates.sort(Qt::CaseInsensitive);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New private dialog"));

    auto* layout = new QVBoxLayout(&dialog);
    auto* hintLabel = new QLabel(QStringLiteral("Choose an online user or enter an existing username."), &dialog);
    hintLabel->setWordWrap(true);

    auto* formLayout = new QFormLayout();
    auto* onlineUsersBox = new QComboBox(&dialog);
    onlineUsersBox->addItem(QString());
    for (const QString& candidate : candidates) {
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

    if (username == m_username) {
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

    ensurePrivateDialog(username);
    rebuildDialogList(username);
    requestCurrentHistory();
    setStatusText(QStringLiteral("Dialog with %1 selected").arg(username));
}
void ChatWindow::sendMessage()
{
    if (!m_onlineSession || m_offlineMode || m_username.isEmpty()) {
        return;
    }

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
    m_onlineUsers = users;
    rebuildOnlineUsersList();
}

void ChatWindow::onDialogsReceived(const QStringList& dialogs)
{
    const QString selectedChat = currentChatKey();
    for (const QString& dialog : dialogs) {
        ensurePrivateDialog(dialog);
    }
    rebuildDialogList(selectedChat);
}

void ChatWindow::onHistoryReceived(const QString& chatUser, const QJsonArray& items)
{
    ensurePrivateDialog(chatUser);

    for (const QJsonValue& value : items) {
        const QJsonObject object = value.toObject();
        ChatMessage message;
        message.id = object.value("id").toString();
        message.chatKey = chatUser;
        message.author = object.value("from").toString();
        message.text = object.value("text").toString();
        message.status = object.value("status").toString(QStringLiteral("delivered"));
        message.outgoing = message.author == m_username;
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

    ensurePrivateDialog(username);
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
        from == m_username,
        createdAt
    });
}

void ChatWindow::onPrivateMessage(const QString& id, const QString& from, const QString& text, qint64 createdAt)
{
    const QString selectedChat = currentChatKey();
    ensurePrivateDialog(from);
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
    if (!m_messagesById.contains(id)) {
        return;
    }

    ChatMessage& message = m_messagesById[id];
    message.status = QStringLiteral("delivered");
    if (createdAt > 0) {
        message.createdAt = createdAt;
    }
    if (!m_username.isEmpty()) {
        m_historyStore->saveMessage(m_username, message);
    }
    refreshCurrentChat();
}

void ChatWindow::onMessageRead(const QString& id, const QString&)
{
    if (!m_messagesById.contains(id)) {
        return;
    }

    ChatMessage& message = m_messagesById[id];
    message.status = QStringLiteral("read");
    if (!m_username.isEmpty()) {
        m_historyStore->saveMessage(m_username, message);
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

    const QStringList ids = m_chatMessages.value(chatKey);
    for (const QString& id : ids) {
        const ChatMessage& message = m_messagesById[id];
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
    m_changeServerButton = new QPushButton(QStringLiteral("Change server"), this);
    m_logoutButton = new QPushButton(QStringLiteral("Logout"), this);
    headerLayout->addWidget(m_statusLabel, 1);
    headerLayout->addWidget(m_changeServerButton);
    headerLayout->addWidget(m_logoutButton);
    rootLayout->addLayout(headerLayout);

    auto* contentLayout = new QHBoxLayout();
    auto* sidebarLayout = new QVBoxLayout();
    auto* dialogsLabel = new QLabel(QStringLiteral("Dialogs"), this);
    m_dialogsList = new QListWidget(this);
    m_newDialogButton = new QPushButton(QStringLiteral("New dialog"), this);
    auto* onlineUsersLabel = new QLabel(QStringLiteral("Online users"), this);
    m_onlineUsersList = new QListWidget(this);
    m_onlineUsersList->setSelectionMode(QAbstractItemView::NoSelection);

    sidebarLayout->addWidget(dialogsLabel);
    sidebarLayout->addWidget(m_dialogsList, 3);
    sidebarLayout->addWidget(m_newDialogButton);
    sidebarLayout->addSpacing(8);
    sidebarLayout->addWidget(onlineUsersLabel);
    sidebarLayout->addWidget(m_onlineUsersList, 2);

    m_historyView = new QTextEdit(this);
    m_historyView->setReadOnly(true);
    contentLayout->addLayout(sidebarLayout, 1);
    contentLayout->addWidget(m_historyView, 3);
    rootLayout->addLayout(contentLayout, 1);

    auto* composerLayout = new QHBoxLayout();
    m_messageEdit = new QLineEdit(this);
    m_sendButton = new QPushButton(QStringLiteral("Send"), this);
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
    for (const QString& dialog : dialogs) {
        ensurePrivateDialog(dialog);
    }

    const QList<ChatMessage> broadcastMessages = m_historyStore->loadMessages(username, QStringLiteral("Broadcast"));
    for (const ChatMessage& message : broadcastMessages) {
        appendMessage(message, false);
    }

    for (const QString& dialog : dialogs) {
        const QList<ChatMessage> messages = m_historyStore->loadMessages(username, dialog);
        for (const ChatMessage& message : messages) {
            appendMessage(message, false);
        }
    }

    rebuildDialogList(QStringLiteral("Broadcast"));
    rebuildOnlineUsersList();
}

void ChatWindow::appendMessage(const ChatMessage& sourceMessage, bool persist)
{
    ChatMessage message = sourceMessage;
    if (message.createdAt <= 0) {
        message.createdAt = QDateTime::currentMSecsSinceEpoch();
    }

    if (m_messagesById.contains(message.id)) {
        ChatMessage& existing = m_messagesById[message.id];
        if (!message.chatKey.isEmpty()) {
            existing.chatKey = message.chatKey;
        }
        existing.author = message.author;
        existing.text = message.text;
        existing.status = message.status;
        existing.outgoing = message.outgoing;
        existing.createdAt = message.createdAt;
        message = existing;
    } else {
        m_messagesById.insert(message.id, message);
    }

    QStringList& ids = m_chatMessages[message.chatKey];
    if (!ids.contains(message.id)) {
        ids.append(message.id);
        std::stable_sort(ids.begin(), ids.end(), [this](const QString& left, const QString& right) {
            const ChatMessage& leftMessage = m_messagesById[left];
            const ChatMessage& rightMessage = m_messagesById[right];
            if (leftMessage.createdAt == rightMessage.createdAt) {
                return left < right;
            }
            return leftMessage.createdAt < rightMessage.createdAt;
        });
    }

    if (persist && !m_username.isEmpty()) {
        m_historyStore->saveMessage(m_username, message);
    }

    refreshCurrentChat();
}

void ChatWindow::clearLoadedHistory()
{
    m_onlineUsers.clear();
    m_privateDialogs.clear();
    m_messagesById.clear();
    m_chatMessages.clear();
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
    if (!m_onlineSession || m_offlineMode) {
        return;
    }

    const QString chatKey = currentChatKey();
    if (chatKey == QStringLiteral("Broadcast")) {
        return;
    }

    const QStringList ids = m_chatMessages.value(chatKey);
    for (const QString& id : ids) {
        ChatMessage& message = m_messagesById[id];
        if (!message.outgoing && message.status != QStringLiteral("read")) {
            message.status = QStringLiteral("read");
            if (!m_username.isEmpty()) {
                m_historyStore->saveMessage(m_username, message);
            }
            m_client->sendReadReceipt(chatKey, id);
        }
    }
}

void ChatWindow::rebuildDialogList(const QString& preferredChat)
{
    const QString selectedChat = preferredChat.isEmpty() ? currentChatKey() : preferredChat;
    QStringList dialogs = m_privateDialogs;
    dialogs.sort(Qt::CaseInsensitive);

    {
        QSignalBlocker blocker(m_dialogsList);
        m_dialogsList->clear();
        m_dialogsList->addItem(QStringLiteral("Broadcast"));
        for (const QString& chat : dialogs) {
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
    QStringList onlineUsers = m_onlineUsers;
    onlineUsers.removeAll(m_username);
    onlineUsers.removeDuplicates();
    onlineUsers.sort(Qt::CaseInsensitive);

    QSignalBlocker blocker(m_onlineUsersList);
    m_onlineUsersList->clear();
    for (const QString& username : onlineUsers) {
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
    m_changeServerButton->setEnabled(!m_username.isEmpty());
    m_logoutButton->setEnabled(!m_username.isEmpty());
    m_onlineUsersList->setEnabled(canCompose);

    if (m_offlineMode) {
        m_messageEdit->setPlaceholderText(QStringLiteral("Offline history is read-only"));
    } else {
        m_messageEdit->setPlaceholderText(QStringLiteral("Type a message"));
    }
}

