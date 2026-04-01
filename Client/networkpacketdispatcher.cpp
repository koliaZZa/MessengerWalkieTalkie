#include "networkpacketdispatcher.h"

#include "../Shared/authprotocol.h"

NetworkPacketDispatcher::NetworkPacketDispatcher(QObject* parent)
    : QObject(parent)
{
}

NetworkPacketDispatcher::DispatchResult NetworkPacketDispatcher::dispatch(const QJsonObject& packet,
                                                                         QString* errorMessage)
{
    const QString type = packet.value(QStringLiteral("type")).toString();

    if (type == QString::fromLatin1(AuthProtocol::kTypeAuthOk)) {
        QString username;
        AuthProtocol::SessionInfo sessionInfo;
        if (!AuthProtocol::parseAuthOkPacket(packet, username, sessionInfo)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Invalid auth response");
            }
            return DispatchResult::InvalidPacket;
        }

        emit authSucceeded(username, sessionInfo.token, sessionInfo.expiresAt);
        return DispatchResult::Handled;
    }

    if (type == QString::fromLatin1(AuthProtocol::kTypeAuthError)) {
        emit authFailed(packet.value(AuthProtocol::kFieldMessage).toString());
        return DispatchResult::Handled;
    }

    if (type == QString::fromLatin1(AuthProtocol::kTypeSessionInvalid)) {
        emit sessionInvalid(packet.value(AuthProtocol::kFieldMessage).toString());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("users")) {
        QStringList users;
        const QJsonArray values = packet.value(QStringLiteral("list")).toArray();
        for (auto value : values) {
            users.append(value.toString());
        }
        emit usersUpdated(users);
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("dialogs")) {
        QStringList dialogs;
        const QJsonArray values = packet.value(QStringLiteral("list")).toArray();
        for (auto value : values) {
            dialogs.append(value.toString());
        }
        emit dialogsReceived(dialogs);
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("history")) {
        emit historyReceived(packet.value(QStringLiteral("with")).toString(),
                             packet.value(QStringLiteral("items")).toArray());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("user_check_result")) {
        emit userLookupFinished(packet.value(QStringLiteral("username")).toString(),
                                packet.value(QStringLiteral("exists")).toBool(),
                                packet.value(QStringLiteral("online")).toBool());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("message")) {
        emit publicMessageReceived(packet.value(QStringLiteral("id")).toString(),
                                   packet.value(QStringLiteral("from")).toString(),
                                   packet.value(QStringLiteral("text")).toString(),
                                   packet.value(QStringLiteral("created_at")).toVariant().toLongLong());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("private")) {
        emit privateMessageReceived(packet.value(QStringLiteral("id")).toString(),
                                    packet.value(QStringLiteral("from")).toString(),
                                    packet.value(QStringLiteral("text")).toString(),
                                    packet.value(QStringLiteral("created_at")).toVariant().toLongLong());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("queued")) {
        emit messageQueued(packet.value(QStringLiteral("id")).toString(),
                           packet.value(QStringLiteral("to")).toString(),
                           packet.value(QStringLiteral("created_at")).toVariant().toLongLong());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("delivered")) {
        emit messageDelivered(packet.value(QStringLiteral("id")).toString(),
                              packet.value(QStringLiteral("created_at")).toVariant().toLongLong());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("read")) {
        emit messageRead(packet.value(QStringLiteral("id")).toString(),
                         packet.value(QStringLiteral("from")).toString());
        return DispatchResult::Handled;
    }

    if (type == QStringLiteral("error")) {
        emit transportError(packet.value(QStringLiteral("message")).toString());
        return DispatchResult::Handled;
    }

    return DispatchResult::Unhandled;
}
