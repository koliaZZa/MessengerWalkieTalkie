#include <QSignalSpy>
#include <QTest>

#include "../Client/networkpacketdispatcher.h"
#include "../Shared/authprotocol.h"

class NetworkPacketDispatcherTests : public QObject
{
    Q_OBJECT

private slots:
    void authPackets();
    void listAndHistoryPackets();
    void messagePacketsAndErrors();
};

void NetworkPacketDispatcherTests::authPackets()
{
    NetworkPacketDispatcher dispatcher;
    QSignalSpy authOkSpy(&dispatcher, &NetworkPacketDispatcher::authSucceeded);
    QSignalSpy authErrorSpy(&dispatcher, &NetworkPacketDispatcher::authFailed);
    QSignalSpy invalidSpy(&dispatcher, &NetworkPacketDispatcher::sessionInvalid);

    AuthProtocol::SessionInfo sessionInfo;
    sessionInfo.token = QStringLiteral("token-1");
    sessionInfo.expiresAt = 12345;

    QCOMPARE(dispatcher.dispatch(AuthProtocol::makeAuthOkPacket(QStringLiteral("alice"), sessionInfo)),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(authOkSpy.count(), 1);
    QCOMPARE(authOkSpy.at(0).at(0).toString(), QStringLiteral("alice"));
    QCOMPARE(authOkSpy.at(0).at(1).toString(), QStringLiteral("token-1"));
    QCOMPARE(authOkSpy.at(0).at(2).toLongLong(), 12345);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeAuthError)},
                 {AuthProtocol::kFieldMessage, QStringLiteral("bad auth")}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(authErrorSpy.count(), 1);
    QCOMPARE(authErrorSpy.at(0).at(0).toString(), QStringLiteral("bad auth"));

    QCOMPARE(dispatcher.dispatch(AuthProtocol::makeSessionInvalidPacket(QStringLiteral("expired"))),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(invalidSpy.count(), 1);
    QCOMPARE(invalidSpy.at(0).at(0).toString(), QStringLiteral("expired"));

    QString errorMessage;
    QCOMPARE(dispatcher.dispatch(QJsonObject{{QStringLiteral("type"), QString::fromLatin1(AuthProtocol::kTypeAuthOk)}},
                                 &errorMessage),
             NetworkPacketDispatcher::DispatchResult::InvalidPacket);
    QCOMPARE(errorMessage, QStringLiteral("Invalid auth response"));
}

void NetworkPacketDispatcherTests::listAndHistoryPackets()
{
    NetworkPacketDispatcher dispatcher;
    QSignalSpy usersSpy(&dispatcher, &NetworkPacketDispatcher::usersUpdated);
    QSignalSpy dialogsSpy(&dispatcher, &NetworkPacketDispatcher::dialogsReceived);
    QSignalSpy historySpy(&dispatcher, &NetworkPacketDispatcher::historyReceived);
    QSignalSpy lookupSpy(&dispatcher, &NetworkPacketDispatcher::userLookupFinished);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("users")},
                 {QStringLiteral("list"), QJsonArray{QStringLiteral("bob"), QStringLiteral("alice")}}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(usersSpy.count(), 1);
    QCOMPARE(usersSpy.at(0).at(0).toStringList(), QStringList({QStringLiteral("bob"), QStringLiteral("alice")}));

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("dialogs")},
                 {QStringLiteral("list"), QJsonArray{QStringLiteral("Broadcast"), QStringLiteral("bob")}}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(dialogsSpy.count(), 1);
    QCOMPARE(dialogsSpy.at(0).at(0).toStringList(), QStringList({QStringLiteral("Broadcast"), QStringLiteral("bob")}));

    const QJsonArray historyItems{QJsonObject{{QStringLiteral("id"), QStringLiteral("m1")}}};
    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("history")},
                 {QStringLiteral("with"), QStringLiteral("bob")},
                 {QStringLiteral("items"), historyItems}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(historySpy.count(), 1);
    QCOMPARE(historySpy.at(0).at(0).toString(), QStringLiteral("bob"));
    QCOMPARE(historySpy.at(0).at(1).toJsonArray(), historyItems);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("user_check_result")},
                 {QStringLiteral("username"), QStringLiteral("bob")},
                 {QStringLiteral("exists"), true},
                 {QStringLiteral("online"), false}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(lookupSpy.count(), 1);
    QCOMPARE(lookupSpy.at(0).at(0).toString(), QStringLiteral("bob"));
    QCOMPARE(lookupSpy.at(0).at(1).toBool(), true);
    QCOMPARE(lookupSpy.at(0).at(2).toBool(), false);
}

void NetworkPacketDispatcherTests::messagePacketsAndErrors()
{
    NetworkPacketDispatcher dispatcher;
    QSignalSpy publicSpy(&dispatcher, &NetworkPacketDispatcher::publicMessageReceived);
    QSignalSpy privateSpy(&dispatcher, &NetworkPacketDispatcher::privateMessageReceived);
    QSignalSpy queuedSpy(&dispatcher, &NetworkPacketDispatcher::messageQueued);
    QSignalSpy deliveredSpy(&dispatcher, &NetworkPacketDispatcher::messageDelivered);
    QSignalSpy readSpy(&dispatcher, &NetworkPacketDispatcher::messageRead);
    QSignalSpy errorSpy(&dispatcher, &NetworkPacketDispatcher::transportError);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("message")},
                 {QStringLiteral("id"), QStringLiteral("m1")},
                 {QStringLiteral("from"), QStringLiteral("alice")},
                 {QStringLiteral("text"), QStringLiteral("hello")},
                 {QStringLiteral("created_at"), 101}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(publicSpy.count(), 1);
    QCOMPARE(publicSpy.at(0).at(0).toString(), QStringLiteral("m1"));

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("private")},
                 {QStringLiteral("id"), QStringLiteral("m2")},
                 {QStringLiteral("from"), QStringLiteral("bob")},
                 {QStringLiteral("text"), QStringLiteral("secret")},
                 {QStringLiteral("created_at"), 202}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(privateSpy.count(), 1);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("queued")},
                 {QStringLiteral("id"), QStringLiteral("m3")},
                 {QStringLiteral("to"), QStringLiteral("bob")},
                 {QStringLiteral("created_at"), 303}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(queuedSpy.count(), 1);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("delivered")},
                 {QStringLiteral("id"), QStringLiteral("m3")},
                 {QStringLiteral("created_at"), 404}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(deliveredSpy.count(), 1);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("read")},
                 {QStringLiteral("id"), QStringLiteral("m3")},
                 {QStringLiteral("from"), QStringLiteral("bob")}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(readSpy.count(), 1);

    QCOMPARE(dispatcher.dispatch(QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("error")},
                 {QStringLiteral("message"), QStringLiteral("server said no")}
             }),
             NetworkPacketDispatcher::DispatchResult::Handled);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("server said no"));

    QCOMPARE(dispatcher.dispatch(QJsonObject{{QStringLiteral("type"), QStringLiteral("mystery")}}),
             NetworkPacketDispatcher::DispatchResult::Unhandled);
}

QTEST_MAIN(NetworkPacketDispatcherTests)

#include "network_packet_dispatcher_tests.moc"
