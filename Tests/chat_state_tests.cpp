#include <QTest>

#include "../Client/chatstate.h"

class ChatStateTests : public QObject
{
    Q_OBJECT

private slots:
    void dialogListFiltersAndSorts();
    void upsertMessageKeepsOrderingAndMovesChats();
    void markIncomingMessagesReadOnlyTouchesUnreadIncoming();
};

void ChatStateTests::dialogListFiltersAndSorts()
{
    ChatSessionState state;
    state.setUsername(QStringLiteral("alice"));
    state.setOnlineUsers({QStringLiteral("charlie"), QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("bob")});
    state.mergeDialogs({QStringLiteral("charlie"), QStringLiteral("Broadcast"), QStringLiteral("alice"), QStringLiteral("bob")});

    QCOMPARE(state.onlineUsersForDisplay(), QStringList({QStringLiteral("bob"), QStringLiteral("charlie")}));
    QCOMPARE(state.dialogList(), QStringList({QStringLiteral("Broadcast"), QStringLiteral("bob"), QStringLiteral("charlie")}));
}

void ChatStateTests::upsertMessageKeepsOrderingAndMovesChats()
{
    ChatSessionState state;

    state.upsertMessage({
        QStringLiteral("late"),
        QStringLiteral("Broadcast"),
        QStringLiteral("alice"),
        QStringLiteral("later"),
        QStringLiteral("delivered"),
        true,
        200
    });
    state.upsertMessage({
        QStringLiteral("early"),
        QStringLiteral("Broadcast"),
        QStringLiteral("bob"),
        QStringLiteral("earlier"),
        QStringLiteral("delivered"),
        false,
        100
    });

    QList<ChatMessage> broadcastMessages = state.messagesForChat(QStringLiteral("Broadcast"));
    QCOMPARE(broadcastMessages.size(), 2);
    QCOMPARE(broadcastMessages.at(0).id, QStringLiteral("early"));
    QCOMPARE(broadcastMessages.at(1).id, QStringLiteral("late"));

    state.upsertMessage({
        QStringLiteral("late"),
        QStringLiteral("bob"),
        QStringLiteral("alice"),
        QStringLiteral("moved"),
        QStringLiteral("read"),
        true,
        300
    });

    broadcastMessages = state.messagesForChat(QStringLiteral("Broadcast"));
    QCOMPARE(broadcastMessages.size(), 1);
    QCOMPARE(broadcastMessages.constFirst().id, QStringLiteral("early"));

    const QList<ChatMessage> privateMessages = state.messagesForChat(QStringLiteral("bob"));
    QCOMPARE(privateMessages.size(), 1);
    QCOMPARE(privateMessages.constFirst().id, QStringLiteral("late"));
    QCOMPARE(privateMessages.constFirst().text, QStringLiteral("moved"));
    QCOMPARE(privateMessages.constFirst().status, QStringLiteral("read"));
}

void ChatStateTests::markIncomingMessagesReadOnlyTouchesUnreadIncoming()
{
    ChatSessionState state;

    state.upsertMessage({
        QStringLiteral("incoming-unread"),
        QStringLiteral("bob"),
        QStringLiteral("bob"),
        QStringLiteral("hello"),
        QStringLiteral("delivered"),
        false,
        100
    });
    state.upsertMessage({
        QStringLiteral("incoming-read"),
        QStringLiteral("bob"),
        QStringLiteral("bob"),
        QStringLiteral("seen"),
        QStringLiteral("read"),
        false,
        200
    });
    state.upsertMessage({
        QStringLiteral("outgoing"),
        QStringLiteral("bob"),
        QStringLiteral("alice"),
        QStringLiteral("mine"),
        QStringLiteral("delivered"),
        true,
        300
    });

    const QList<ChatMessage> updatedMessages = state.markIncomingMessagesRead(QStringLiteral("bob"));
    QCOMPARE(updatedMessages.size(), 1);
    QCOMPARE(updatedMessages.constFirst().id, QStringLiteral("incoming-unread"));
    QCOMPARE(updatedMessages.constFirst().status, QStringLiteral("read"));

    const QList<ChatMessage> privateMessages = state.messagesForChat(QStringLiteral("bob"));
    QCOMPARE(privateMessages.at(0).status, QStringLiteral("read"));
    QCOMPARE(privateMessages.at(1).status, QStringLiteral("read"));
    QCOMPARE(privateMessages.at(2).status, QStringLiteral("delivered"));
}

QTEST_MAIN(ChatStateTests)

#include "chat_state_tests.moc"
