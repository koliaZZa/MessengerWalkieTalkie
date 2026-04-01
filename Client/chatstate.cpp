#include "chatstate.h"

#include <QDateTime>
#include <algorithm>

namespace {

constexpr char kBroadcastChat[] = "Broadcast";

bool messageComesBefore(const ChatMessage& left, const ChatMessage& right)
{
    if (left.createdAt == right.createdAt) {
        return left.id < right.id;
    }

    return left.createdAt < right.createdAt;
}

}

void ChatSessionState::clear()
{
    m_onlineUsers.clear();
    m_privateDialogs.clear();
    m_messagesById.clear();
    m_chatMessages.clear();
}

void ChatSessionState::setUsername(const QString& username)
{
    m_username = username;
}

QString ChatSessionState::username() const
{
    return m_username;
}

void ChatSessionState::setOnlineUsers(const QStringList& users)
{
    m_onlineUsers = users;
}

QStringList ChatSessionState::onlineUsersForDisplay() const
{
    QStringList users = m_onlineUsers;
    users.removeAll(m_username);
    users.removeDuplicates();
    users.sort(Qt::CaseInsensitive);
    return users;
}

void ChatSessionState::mergeDialogs(const QStringList& dialogs)
{
    for (auto dialog : dialogs) {
        ensurePrivateDialog(dialog);
    }
}

void ChatSessionState::ensurePrivateDialog(const QString& username)
{
    if (username.isEmpty() || username == m_username || username == QString::fromLatin1(kBroadcastChat)) {
        return;
    }

    if (!m_privateDialogs.contains(username)) {
        m_privateDialogs.append(username);
    }
}

QStringList ChatSessionState::dialogList() const
{
    QStringList dialogs = m_privateDialogs;
    dialogs.removeAll(QString::fromLatin1(kBroadcastChat));
    dialogs.removeAll(m_username);
    dialogs.removeDuplicates();
    dialogs.sort(Qt::CaseInsensitive);
    dialogs.prepend(QString::fromLatin1(kBroadcastChat));
    return dialogs;
}

ChatMessage ChatSessionState::upsertMessage(const ChatMessage& sourceMessage)
{
    ChatMessage message = sourceMessage;
    if (message.createdAt <= 0) {
        message.createdAt = QDateTime::currentMSecsSinceEpoch();
    }

    const ChatMessage existing = m_messagesById.value(message.id);
    const QString previousChatKey = existing.chatKey;

    if (m_messagesById.contains(message.id)) {
        ChatMessage& stored = m_messagesById[message.id];
        if (!message.chatKey.isEmpty()) {
            stored.chatKey = message.chatKey;
        }
        stored.author = message.author;
        stored.text = message.text;
        stored.status = message.status;
        stored.outgoing = message.outgoing;
        stored.createdAt = message.createdAt;
        message = stored;
    } else {
        m_messagesById.insert(message.id, message);
    }

    if (!previousChatKey.isEmpty() && previousChatKey != message.chatKey) {
        m_chatMessages[previousChatKey].removeAll(message.id);
    }

    QStringList& ids = m_chatMessages[message.chatKey];
    if (!ids.contains(message.id)) {
        ids.append(message.id);
    }
    sortChatMessages(message.chatKey);

    return m_messagesById.value(message.id);
}

bool ChatSessionState::containsMessage(const QString& id) const
{
    return m_messagesById.contains(id);
}

ChatMessage ChatSessionState::messageById(const QString& id) const
{
    return m_messagesById.value(id);
}

bool ChatSessionState::updateMessageStatus(const QString& id,
                                           const QString& status,
                                           qint64 createdAt,
                                           ChatMessage* updatedMessage)
{
    if (!m_messagesById.contains(id)) {
        return false;
    }

    ChatMessage& message = m_messagesById[id];
    message.status = status;
    if (createdAt > 0) {
        message.createdAt = createdAt;
        sortChatMessages(message.chatKey);
    }

    if (updatedMessage) {
        *updatedMessage = message;
    }

    return true;
}

QList<ChatMessage> ChatSessionState::messagesForChat(const QString& chatKey) const
{
    QList<ChatMessage> messages;
    const QStringList ids = m_chatMessages.value(chatKey);
    for (auto id : ids) {
        messages.append(m_messagesById.value(id));
    }
    return messages;
}

QList<ChatMessage> ChatSessionState::markIncomingMessagesRead(const QString& chatKey)
{
    QList<ChatMessage> updatedMessages;
    QStringList& ids = m_chatMessages[chatKey];
    for (auto id : ids) {
        ChatMessage& message = m_messagesById[id];
        if (message.outgoing || message.status == QStringLiteral("read")) {
            continue;
        }

        message.status = QStringLiteral("read");
        updatedMessages.append(message);
    }

    return updatedMessages;
}

void ChatSessionState::sortChatMessages(const QString& chatKey)
{
    QStringList& ids = m_chatMessages[chatKey];
    std::stable_sort(ids.begin(), ids.end(), [this](const QString& left, const QString& right) {
        return messageComesBefore(m_messagesById[left], m_messagesById[right]);
    });
}
