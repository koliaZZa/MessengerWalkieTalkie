#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace AuthProtocol {

inline constexpr int kMinUsernameLength = 3;
inline constexpr int kMaxUsernameLength = 32;
inline constexpr int kMinPasswordLength = 8;
inline constexpr int kMaxMessageLength = 4000;
inline constexpr int kSessionTokenBytes = 32;
inline constexpr qint64 kSessionLifetimeMs = 7LL * 24 * 60 * 60 * 1000;

inline constexpr char kTypeAuthOk[] = "auth_ok";
inline constexpr char kTypeAuthError[] = "auth_error";
inline constexpr char kTypeLogin[] = "login";
inline constexpr char kTypeRegister[] = "register";
inline constexpr char kTypeResumeSession[] = "resume_session";
inline constexpr char kTypeSessionInvalid[] = "session_invalid";
inline constexpr char kTypeLogout[] = "logout";

inline constexpr char kFieldType[] = "type";
inline constexpr char kFieldUsername[] = "username";
inline constexpr char kFieldPassword[] = "password";
inline constexpr char kFieldMessage[] = "message";
inline constexpr char kFieldSessionToken[] = "session_token";
inline constexpr char kFieldSessionExpiresAt[] = "session_expires_at";

struct SessionInfo {
    QString token;
    qint64 expiresAt = 0;

    bool isValid() const
    {
        return !token.isEmpty() && expiresAt > 0;
    }
};

inline QString normalizeUsername(const QString& username)
{
    return username.trimmed();
}

inline bool isAsciiUsernameValid(const QString& username)
{
    if (username.size() < kMinUsernameLength || username.size() > kMaxUsernameLength) {
        return false;
    }

    for (auto character : username) {
        const ushort code = character.unicode();
        if (code > 0x00FF) {
            return false;
        }

        const bool isDigit = code >= '0' && code <= '9';
        const bool isUpper = code >= 'A' && code <= 'Z';
        const bool isLower = code >= 'a' && code <= 'z';
        if (!(isDigit || isUpper || isLower || code == '_')) {
            return false;
        }
    }

    return true;
}

inline bool isMessageTextValid(const QString& text)
{
    const QString trimmed = text.trimmed();
    return !trimmed.isEmpty() && trimmed.size() <= kMaxMessageLength;
}

inline bool isPrivateRecipientValid(const QString& senderUsername, const QString& recipientUsername)
{
    const QString normalizedSender = normalizeUsername(senderUsername);
    const QString normalizedRecipient = normalizeUsername(recipientUsername);
    return !normalizedSender.isEmpty()
        && !normalizedRecipient.isEmpty()
        && normalizedSender != normalizedRecipient;
}

inline QJsonObject makeAuthOkPacket(const QString& username, const SessionInfo& session)
{
    return {
        {kFieldType, QString::fromLatin1(kTypeAuthOk)},
        {kFieldUsername, username},
        {kFieldSessionToken, session.token},
        {kFieldSessionExpiresAt, session.expiresAt}
    };
}

inline bool parseAuthOkPacket(const QJsonObject& packet, QString& username, SessionInfo& session)
{
    if (packet.value(kFieldType).toString() != QString::fromLatin1(kTypeAuthOk)) {
        return false;
    }

    username = normalizeUsername(packet.value(kFieldUsername).toString());
    session.token = packet.value(kFieldSessionToken).toString();
    session.expiresAt = packet.value(kFieldSessionExpiresAt).toVariant().toLongLong();
    return !username.isEmpty() && session.isValid();
}

inline QJsonObject makeResumeSessionPacket(const QString& username, const QString& sessionToken)
{
    return {
        {kFieldType, QString::fromLatin1(kTypeResumeSession)},
        {kFieldUsername, normalizeUsername(username)},
        {kFieldSessionToken, sessionToken}
    };
}

inline QJsonObject makeSessionInvalidPacket(const QString& message)
{
    return {
        {kFieldType, QString::fromLatin1(kTypeSessionInvalid)},
        {kFieldMessage, message}
    };
}

inline QJsonObject makeLogoutPacket()
{
    return {
        {kFieldType, QString::fromLatin1(kTypeLogout)}
    };
}

}  // namespace AuthProtocol



