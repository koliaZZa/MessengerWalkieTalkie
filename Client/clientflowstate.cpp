#include "clientflowstate.h"

void ClientFlowState::restoreLastSession(const LastSessionInfo& lastSession)
{
    m_lastSession = lastSession;
    m_pendingUsername.clear();
    m_activeUsername.clear();
    m_sessionResumePending = false;
    m_sessionResumeBlocked = false;
    m_offlineViewActive = false;

    if (m_lastSession.hasStoredIdentity()) {
        setEndpoint(m_lastSession.host, m_lastSession.port);
    }
}

void ClientFlowState::setEndpoint(const QString& host, quint16 port)
{
    m_currentHost = normalizedHost(host);
    m_currentPort = normalizedPort(port);
}

QString ClientFlowState::currentHost() const
{
    return m_currentHost;
}

quint16 ClientFlowState::currentPort() const
{
    return m_currentPort;
}

const LastSessionInfo& ClientFlowState::lastSession() const
{
    return m_lastSession;
}

bool ClientFlowState::hasStoredSession() const
{
    return m_lastSession.hasStoredIdentity();
}

bool ClientFlowState::hasActiveSession() const
{
    return !m_activeUsername.isEmpty();
}

QString ClientFlowState::activeUsername() const
{
    return m_activeUsername;
}

QString ClientFlowState::loginPrefillUsername() const
{
    return m_pendingUsername.isEmpty() ? m_lastSession.username : m_pendingUsername;
}

bool ClientFlowState::isOfflineViewActive() const
{
    return m_offlineViewActive;
}

bool ClientFlowState::expireStoredSessionIfNeeded(qint64 nowMs)
{
    if (!m_lastSession.hasStoredIdentity()
        || m_lastSession.sessionToken.isEmpty()
        || m_lastSession.sessionExpiresAt <= 0
        || m_lastSession.sessionExpiresAt > nowMs) {
        return false;
    }

    clearStoredSessionToken();
    return true;
}

bool ClientFlowState::canResumeSession(qint64 nowMs) const
{
    if (!m_lastSession.hasSessionToken() || m_sessionResumeBlocked) {
        return false;
    }

    return m_lastSession.sessionExpiresAt > nowMs;
}

void ClientFlowState::beginSessionResume()
{
    if (!m_lastSession.hasStoredIdentity()) {
        return;
    }

    m_sessionResumePending = true;
    m_pendingUsername = m_lastSession.username;
}

void ClientFlowState::markCredentialAuthRequested(const QString& username)
{
    m_pendingUsername = username;
    m_sessionResumePending = false;
    m_sessionResumeBlocked = false;
}

bool ClientFlowState::handleSocketDisconnected(bool chatVisible)
{
    if (chatVisible && hasActiveSession()) {
        m_offlineViewActive = true;
        return true;
    }

    return false;
}

bool ClientFlowState::activateOfflineView()
{
    if (!hasStoredSession()) {
        return false;
    }

    if (m_activeUsername.isEmpty()) {
        m_activeUsername = m_lastSession.username;
    }

    m_offlineViewActive = true;
    return true;
}

void ClientFlowState::handleAuthSucceeded(const QString& username,
                                          const QString& sessionToken,
                                          qint64 sessionExpiresAt)
{
    LastSessionInfo session;
    session.username = username;
    session.host = m_currentHost;
    session.port = m_currentPort;
    session.sessionToken = sessionToken;
    session.sessionExpiresAt = sessionExpiresAt;

    m_lastSession = session;
    m_pendingUsername = username;
    m_activeUsername = username;
    m_sessionResumePending = false;
    m_sessionResumeBlocked = false;
    m_offlineViewActive = false;
}

void ClientFlowState::handleAuthFailed()
{
    m_sessionResumePending = false;
}

void ClientFlowState::handleSessionInvalid()
{
    m_sessionResumePending = false;
    m_sessionResumeBlocked = true;
    clearStoredSessionToken();
}

void ClientFlowState::handleEndpointChange(const QString& host, quint16 port, bool endpointChanged)
{
    setEndpoint(host, port);
    m_sessionResumePending = false;
    m_sessionResumeBlocked = endpointChanged;
    m_offlineViewActive = false;
}

void ClientFlowState::handleLogout()
{
    m_lastSession = LastSessionInfo{};
    m_pendingUsername.clear();
    m_activeUsername.clear();
    m_sessionResumePending = false;
    m_sessionResumeBlocked = false;
    m_offlineViewActive = false;
}

void ClientFlowState::clearStoredSessionToken()
{
    if (!m_lastSession.hasStoredIdentity()) {
        return;
    }

    m_lastSession.sessionToken.clear();
    m_lastSession.sessionExpiresAt = 0;
}

QString ClientFlowState::normalizedHost(const QString& host)
{
    const QString trimmed = host.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("127.0.0.1") : trimmed;
}

quint16 ClientFlowState::normalizedPort(quint16 port)
{
    return port == 0 ? 5555 : port;
}
