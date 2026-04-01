#pragma once

#include <QString>

#include "historystore.h"

class ClientFlowState
{
public:
    void restoreLastSession(const LastSessionInfo& lastSession);
    void setEndpoint(const QString& host, quint16 port);

    QString currentHost() const;
    quint16 currentPort() const;

    const LastSessionInfo& lastSession() const;
    bool hasStoredSession() const;
    bool hasActiveSession() const;
    QString activeUsername() const;
    QString loginPrefillUsername() const;
    bool isOfflineViewActive() const;

    bool expireStoredSessionIfNeeded(qint64 nowMs);
    bool canResumeSession(qint64 nowMs) const;
    void beginSessionResume();
    void markCredentialAuthRequested(const QString& username);
    bool handleSocketDisconnected(bool chatVisible);
    bool activateOfflineView();
    void handleAuthSucceeded(const QString& username,
                             const QString& sessionToken,
                             qint64 sessionExpiresAt);
    void handleAuthFailed();
    void handleSessionInvalid();
    void handleEndpointChange(const QString& host, quint16 port, bool endpointChanged);
    void handleLogout();
    void clearStoredSessionToken();

private:
    static QString normalizedHost(const QString& host);
    static quint16 normalizedPort(quint16 port);

    LastSessionInfo m_lastSession;
    QString m_currentHost {QStringLiteral("127.0.0.1")};
    quint16 m_currentPort {5555};
    QString m_pendingUsername;
    QString m_activeUsername;
    bool m_sessionResumePending {false};
    bool m_sessionResumeBlocked {false};
    bool m_offlineViewActive {false};
};
