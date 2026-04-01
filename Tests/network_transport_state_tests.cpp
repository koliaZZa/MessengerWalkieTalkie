#include <QTest>

#include "../Client/networktransportstate.h"

class NetworkTransportStateTests : public QObject
{
    Q_OBJECT

private slots:
    void connectAndReconnectFlow();
    void reliableRetryAndAckFlow();
    void manualDisconnectAndHeartbeat();
};

void NetworkTransportStateTests::connectAndReconnectFlow()
{
    NetworkTransportState state;

    state.startConnection(QString(), 0);
    QCOMPARE(state.host(), QStringLiteral("127.0.0.1"));
    QCOMPARE(state.port(), static_cast<quint16>(5555));
    QVERIFY(state.canReconnect());
    QCOMPARE(state.scheduleReconnect(), 1000);
    QCOMPARE(state.scheduleReconnect(), 2000);

    state.markConnected(5000);
    QCOMPARE(state.lastPongAtMs(), 5000);
    QCOMPARE(state.scheduleReconnect(), 1000);
}

void NetworkTransportStateTests::reliableRetryAndAckFlow()
{
    NetworkTransportState state;
    state.startConnection(QStringLiteral("chat.example"), 7777);

    QJsonObject packet{{QStringLiteral("type"), QStringLiteral("private")}, {QStringLiteral("id"), QStringLiteral("msg-1")}};
    packet.insert(QStringLiteral("seq"), static_cast<qint64>(state.nextOutgoingSequence()));
    state.trackReliableMessage(QStringLiteral("msg-1"), packet, 1000);

    QCOMPARE(state.pendingReliableCount(), 1);
    QVERIFY(!state.isDuplicateIncomingSequence(1));
    state.markIncomingSequence(1);
    QVERIFY(state.isDuplicateIncomingSequence(1));
    QVERIFY(!state.isDuplicateIncomingSequence(2));

    RetryActions actions = state.collectRetryActions(2999);
    QVERIFY(actions.resendPackets.isEmpty());
    QVERIFY(actions.droppedMessageIds.isEmpty());

    actions = state.collectRetryActions(3000);
    QCOMPARE(actions.resendPackets.size(), 1);
    QCOMPARE(actions.resendPackets.constFirst().value(QStringLiteral("id")).toString(), QStringLiteral("msg-1"));
    QVERIFY(actions.droppedMessageIds.isEmpty());

    state.acknowledgeReliableMessage(QStringLiteral("msg-1"));
    QCOMPARE(state.pendingReliableCount(), 0);
    actions = state.collectRetryActions(7000);
    QVERIFY(actions.resendPackets.isEmpty());
    QVERIFY(actions.droppedMessageIds.isEmpty());
}

void NetworkTransportStateTests::manualDisconnectAndHeartbeat()
{
    NetworkTransportState state;
    state.startConnection(QStringLiteral("srv"), 6000);
    state.incomingBuffer().append("partial");
    QJsonObject packet{{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("id"), QStringLiteral("msg-2")}};
    state.trackReliableMessage(QStringLiteral("msg-2"), packet, 1000);

    state.prepareDisconnect();
    QVERIFY(!state.canReconnect());
    QVERIFY(state.consumeManualDisconnect());
    QVERIFY(!state.consumeManualDisconnect());
    QVERIFY(state.incomingBuffer().isEmpty());
    QCOMPARE(state.pendingReliableCount(), 0);

    state.startConnection(QStringLiteral("srv"), 6000);
    state.markConnected(10'000);
    QVERIFY(!state.isHeartbeatExpired(24'999));
    QVERIFY(state.isHeartbeatExpired(25'001));
    state.markPongReceived(30'000);
    QVERIFY(!state.isHeartbeatExpired(44'999));
}

QTEST_MAIN(NetworkTransportStateTests)

#include "network_transport_state_tests.moc"
