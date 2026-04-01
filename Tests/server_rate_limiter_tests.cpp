#include <QCoreApplication>
#include <QTest>

#include "../Server/ratelimiter.h"

class ServerRateLimiterTests : public QObject
{
    Q_OBJECT

private slots:
    void allowsHitsWithinWindow();
    void rejectsHitAboveLimitWithRetryAfter();
    void resetsAfterWindowExpires();
    void tracksKeysIndependently();
};

void ServerRateLimiterTests::allowsHitsWithinWindow()
{
    RateLimiter limiter;
    const RateLimiter::Rule rule{3, 1000};

    const auto first = limiter.allow(QStringLiteral("alpha"), rule, 1000);
    QVERIFY(first.allowed);
    QCOMPARE(first.remainingHits, 2);

    const auto second = limiter.allow(QStringLiteral("alpha"), rule, 1100);
    QVERIFY(second.allowed);
    QCOMPARE(second.remainingHits, 1);

    const auto third = limiter.allow(QStringLiteral("alpha"), rule, 1200);
    QVERIFY(third.allowed);
    QCOMPARE(third.remainingHits, 0);
}

void ServerRateLimiterTests::rejectsHitAboveLimitWithRetryAfter()
{
    RateLimiter limiter;
    const RateLimiter::Rule rule{2, 1000};

    QVERIFY(limiter.allow(QStringLiteral("alpha"), rule, 1000).allowed);
    QVERIFY(limiter.allow(QStringLiteral("alpha"), rule, 1200).allowed);

    const auto rejected = limiter.allow(QStringLiteral("alpha"), rule, 1500);
    QVERIFY(!rejected.allowed);
    QCOMPARE(rejected.remainingHits, 0);
    QCOMPARE(rejected.retryAfterMs, 500);
}

void ServerRateLimiterTests::resetsAfterWindowExpires()
{
    RateLimiter limiter;
    const RateLimiter::Rule rule{2, 1000};

    QVERIFY(limiter.allow(QStringLiteral("alpha"), rule, 1000).allowed);
    QVERIFY(limiter.allow(QStringLiteral("alpha"), rule, 1200).allowed);
    QVERIFY(!limiter.allow(QStringLiteral("alpha"), rule, 1500).allowed);

    const auto reset = limiter.allow(QStringLiteral("alpha"), rule, 2001);
    QVERIFY(reset.allowed);
    QCOMPARE(reset.remainingHits, 1);
}

void ServerRateLimiterTests::tracksKeysIndependently()
{
    RateLimiter limiter;
    const RateLimiter::Rule rule{1, 1000};

    QVERIFY(limiter.allow(QStringLiteral("alpha"), rule, 1000).allowed);
    QVERIFY(!limiter.allow(QStringLiteral("alpha"), rule, 1100).allowed);

    const auto otherKey = limiter.allow(QStringLiteral("beta"), rule, 1100);
    QVERIFY(otherKey.allowed);
    QCOMPARE(otherKey.remainingHits, 0);
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("server_rate_limiter_tests"));
    ServerRateLimiterTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "server_rate_limiter_tests.moc"
