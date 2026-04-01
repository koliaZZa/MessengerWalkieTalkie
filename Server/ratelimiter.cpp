#include "ratelimiter.h"

#include <QDateTime>

namespace {

constexpr int kPruneThreshold = 512;

}

RateLimiter::Decision RateLimiter::allow(const QString& key, const Rule& rule, qint64 nowMs)
{
    Decision decision;
    if (key.isEmpty() || rule.maxHits <= 0 || rule.windowMs <= 0) {
        return decision;
    }

    if (m_entries.size() >= kPruneThreshold) {
        pruneExpiredEntries(nowMs);
    }

    Entry& entry = m_entries[key];
    if (entry.windowStartedAtMs <= 0 || nowMs - entry.windowStartedAtMs >= rule.windowMs) {
        entry.hits = 1;
        entry.windowStartedAtMs = nowMs;
        entry.lastSeenAtMs = nowMs;
        decision.remainingHits = qMax(0, rule.maxHits - entry.hits);
        return decision;
    }

    entry.lastSeenAtMs = nowMs;
    if (entry.hits >= rule.maxHits) {
        decision.allowed = false;
        decision.remainingHits = 0;
        decision.retryAfterMs = qMax<qint64>(1, rule.windowMs - (nowMs - entry.windowStartedAtMs));
        return decision;
    }

    ++entry.hits;
    decision.remainingHits = qMax(0, rule.maxHits - entry.hits);
    return decision;
}

RateLimiter::Decision RateLimiter::allow(const QString& key, const Rule& rule)
{
    return allow(key, rule, QDateTime::currentMSecsSinceEpoch());
}

void RateLimiter::pruneExpiredEntries(qint64 nowMs)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        const Entry& entry = it.value();
        if (entry.lastSeenAtMs <= 0 || nowMs - entry.lastSeenAtMs > 10 * 60 * 1000) {
            it = m_entries.erase(it);
            continue;
        }
        ++it;
    }
}
