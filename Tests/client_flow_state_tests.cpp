#include <QTest>

#include "../Client/clientflowstate.h"

class ClientFlowStateTests : public QObject
{
    Q_OBJECT

private slots:
    void restoreAndResumeFlow();
    void expiredAndInvalidSessionsClearToken();
    void offlineModeEndpointChangeAndLogout();
};

void ClientFlowStateTests::restoreAndResumeFlow()
{
    ClientFlowState state;
    LastSessionInfo session;
    session.username = QStringLiteral("alice");
    session.host = QStringLiteral("chat.example");
    session.port = 6789;
    session.sessionToken = QStringLiteral("resume-token");
    session.sessionExpiresAt = 1'500;

    state.restoreLastSession(session);

    QCOMPARE(state.currentHost(), QStringLiteral("chat.example"));
    QCOMPARE(state.currentPort(), static_cast<quint16>(6789));
    QVERIFY(state.hasStoredSession());
    QVERIFY(state.canResumeSession(1'000));

    state.beginSessionResume();
    QCOMPARE(state.loginPrefillUsername(), QStringLiteral("alice"));

    state.handleAuthSucceeded(QStringLiteral("alice"), QStringLiteral("fresh-token"), 2'000);

    QVERIFY(state.hasActiveSession());
    QCOMPARE(state.activeUsername(), QStringLiteral("alice"));
    QCOMPARE(state.lastSession().sessionToken, QStringLiteral("fresh-token"));
    QCOMPARE(state.lastSession().sessionExpiresAt, 2'000);
    QVERIFY(!state.isOfflineViewActive());
}

void ClientFlowStateTests::expiredAndInvalidSessionsClearToken()
{
    ClientFlowState state;
    LastSessionInfo session;
    session.username = QStringLiteral("alice");
    session.host = QStringLiteral("127.0.0.1");
    session.port = 5555;
    session.sessionToken = QStringLiteral("stale-token");
    session.sessionExpiresAt = 100;

    state.restoreLastSession(session);
    QVERIFY(state.expireStoredSessionIfNeeded(100));
    QVERIFY(!state.lastSession().hasSessionToken());
    QVERIFY(state.hasStoredSession());
    QVERIFY(!state.canResumeSession(50));

    session.sessionToken = QStringLiteral("resume-token");
    session.sessionExpiresAt = 500;
    state.restoreLastSession(session);
    QVERIFY(state.canResumeSession(200));

    state.handleSessionInvalid();
    QVERIFY(!state.lastSession().hasSessionToken());
    QVERIFY(!state.canResumeSession(200));
    QCOMPARE(state.loginPrefillUsername(), QStringLiteral("alice"));
}

void ClientFlowStateTests::offlineModeEndpointChangeAndLogout()
{
    ClientFlowState state;
    state.setEndpoint(QStringLiteral("server-a"), 6000);
    state.handleAuthSucceeded(QStringLiteral("alice"), QStringLiteral("token"), 2'000);

    QVERIFY(state.handleSocketDisconnected(true));
    QVERIFY(state.isOfflineViewActive());
    QCOMPARE(state.activeUsername(), QStringLiteral("alice"));

    state.handleEndpointChange(QStringLiteral("server-b"), 7000, true);
    QCOMPARE(state.currentHost(), QStringLiteral("server-b"));
    QCOMPARE(state.currentPort(), static_cast<quint16>(7000));
    QVERIFY(!state.isOfflineViewActive());
    QVERIFY(!state.canResumeSession(100));

    state.handleLogout();
    QVERIFY(!state.hasStoredSession());
    QVERIFY(!state.hasActiveSession());
    QCOMPARE(state.currentHost(), QStringLiteral("server-b"));
    QCOMPARE(state.currentPort(), static_cast<quint16>(7000));
    QCOMPARE(state.loginPrefillUsername(), QString());
}

QTEST_MAIN(ClientFlowStateTests)

#include "client_flow_state_tests.moc"
