#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

class RateLimiter
{
public:
    struct Rule {
        int maxHits = 0;
        qint64 windowMs = 0;
    };

    struct Decision {
        bool allowed = true;
        int remainingHits = 0;
        qint64 retryAfterMs = 0;
    };

    Decision allow(const QString& key, const Rule& rule, qint64 nowMs);
    Decision allow(const QString& key, const Rule& rule);

private:
    struct Entry {
        int hits = 0;
        qint64 windowStartedAtMs = 0;
        qint64 lastSeenAtMs = 0;
    };

    void pruneExpiredEntries(qint64 nowMs);

    QHash<QString, Entry> m_entries;
};
