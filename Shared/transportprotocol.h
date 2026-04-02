#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>

namespace Protocol {

inline constexpr qsizetype kHeaderSize = 4;
inline constexpr qsizetype kFooterSize = 4;
inline constexpr qsizetype kMaxPayloadSize = 256 * 1024;

enum class DecodeStatus {
    NeedMoreData,
    Ok,
    InvalidPacket,
    CrcMismatch,
    InvalidJson
};

inline quint32 crc32(const QByteArray& data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (auto byte : data) {
        crc ^= static_cast<quint32>(byte);
        for (int i = 0; i < 8; ++i) {
            const quint32 mask = -(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

inline QByteArray encodePacket(const QJsonObject& object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const quint32 payloadSize = static_cast<quint32>(payload.size());
    const quint32 payloadCrc = crc32(payload);

    QByteArray packet;
    packet.reserve(static_cast<int>(kHeaderSize + payload.size() + kFooterSize));

    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << payloadSize;
    packet.append(payload);
    stream.device()->seek(packet.size());
    stream << payloadCrc;

    return packet;
}

inline bool writePacket(QIODevice* device, const QJsonObject& object)
{
    if (!device) {
        return false;
    }

    const QByteArray packet = encodePacket(object);
    return device->write(packet) == packet.size();
}

inline DecodeStatus tryDecode(QByteArray& buffer, QJsonObject& outObject)
{
    if (buffer.size() < kHeaderSize + kFooterSize) {
        return DecodeStatus::NeedMoreData;
    }

    QByteArray headerBytes = buffer.first(kHeaderSize);
    QDataStream headerStream(&headerBytes, QIODevice::ReadOnly);
    headerStream.setVersion(QDataStream::Qt_6_0);

    quint32 payloadSize = 0;
    headerStream >> payloadSize;
    if (headerStream.status() != QDataStream::Ok || payloadSize > static_cast<quint32>(kMaxPayloadSize)) {
        buffer.clear();
        return DecodeStatus::InvalidPacket;
    }

    const qsizetype fullPacketSize = kHeaderSize + static_cast<qsizetype>(payloadSize) + kFooterSize;
    if (buffer.size() < fullPacketSize) {
        return DecodeStatus::NeedMoreData;
    }

    const QByteArray packet = buffer.first(fullPacketSize);
    buffer.remove(0, fullPacketSize);

    const QByteArray payload = packet.mid(kHeaderSize, payloadSize);
    QByteArray footerBytes = packet.last(kFooterSize);
    QDataStream footerStream(&footerBytes, QIODevice::ReadOnly);
    footerStream.setVersion(QDataStream::Qt_6_0);

    quint32 expectedCrc = 0;
    footerStream >> expectedCrc;
    if (footerStream.status() != QDataStream::Ok) {
        return DecodeStatus::InvalidPacket;
    }

    if (crc32(payload) != expectedCrc) {
        return DecodeStatus::CrcMismatch;
    }

    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        return DecodeStatus::InvalidJson;
    }

    outObject = document.object();
    return DecodeStatus::Ok;
}

}  // namespace Protocol
