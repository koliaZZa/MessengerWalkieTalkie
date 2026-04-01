#pragma once

#include <QHash>
#include <QList>
#include <QStringList>

#include "historystore.h"

class ChatSessionState
{
public:
    void clear();

    void setUsername(const QString& username);
    QString username() const;

    void setOnlineUsers(const QStringList& users);
    QStringList onlineUsersForDisplay() const;

    void mergeDialogs(const QStringList& dialogs);
    void ensurePrivateDialog(const QString& username);
    QStringList dialogList() const;

    ChatMessage upsertMessage(const ChatMessage& message);
    bool containsMessage(const QString& id) const;
    ChatMessage messageById(const QString& id) const;
    bool updateMessageStatus(const QString& id,
                             const QString& status,
                             qint64 createdAt = 0,
                             ChatMessage* updatedMessage = nullptr);
    QList<ChatMessage> messagesForChat(const QString& chatKey) const;
    QList<ChatMessage> markIncomingMessagesRead(const QString& chatKey);

private:
    void sortChatMessages(const QString& chatKey);

    QString m_username;
    QStringList m_onlineUsers;
    QStringList m_privateDialogs;
    QHash<QString, ChatMessage> m_messagesById;
    QHash<QString, QStringList> m_chatMessages;
};
