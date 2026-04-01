#include <functional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTest>
#include <QUuid>

#include "sqlite_test_support.h"
#include "../Server/server.h"
#include "../Shared/authprotocol.h"
#include "../Shared/transportprotocol.h"

namespace {

QString uniqueId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

class TestSocketClient
{
public:
    bool connectTo(quint16 port, const QString& host = QStringLiteral("127.0.0.1"), int timeoutMs = 5000)
    {
        m_socket.connectToHost(host, port);
        const bool connected = m_socket.waitForConnected(timeoutMs);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        return connected;
    }

    void disconnect()
    {
        m_socket.disconnectFromHost();
        if (m_socket.state() != QAbstractSocket::UnconnectedState) {
            m_socket.waitForDisconnected(3000);
        }
        m_packets.clear();
        m_buffer.clear();
    }

    bool waitForDisconnected(int timeoutMs = 3000)
    {
        QElapsedTimer timer;
        timer.start();

        while (timer.elapsed() < timeoutMs) {
            if (m_socket.state() == QAbstractSocket::UnconnectedState) {
                return true;
            }

            const int waitChunkMs = qMax(1, qMin(100, timeoutMs - static_cast<int>(timer.elapsed())));
            QCoreApplication::processEvents(QEventLoop::AllEvents, waitChunkMs);
            if (m_socket.waitForDisconnected(waitChunkMs)) {
                return true;
            }
        }

        return m_socket.state() == QAbstractSocket::UnconnectedState;
    }

    bool send(const QJsonObject& packet)
    {
        const bool ok = Protocol::writePacket(&m_socket, packet);
        m_socket.flush();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return ok;
    }

    QString sendReliable(QJsonObject packet)
    {
        QString id = packet.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            id = uniqueId();
            packet.insert(QStringLiteral("id"), id);
        }

        packet.insert(QStringLiteral("seq"), static_cast<qint64>(++m_nextOutgoingSeq));
        send(packet);
        return id;
    }

    bool waitForPacketType(const QString& type, QJsonObject* outPacket, int timeoutMs = 5000)
    {
        return waitForPacket([&](const QJsonObject& packet) {
            return packet.value(QStringLiteral("type")).toString() == type;
        }, outPacket, timeoutMs);
    }

private:
    bool waitForPacket(const std::function<bool(const QJsonObject&)>& predicate,
                       QJsonObject* outPacket,
                       int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();

        while (timer.elapsed() < timeoutMs) {
            for (auto it = m_packets.begin(); it != m_packets.end(); ++it) {
                if (!predicate(*it)) {
                    continue;
                }

                if (outPacket) {
                    *outPacket = *it;
                }
                m_packets.erase(it);
                return true;
            }

            const int waitChunkMs = qMax(1, qMin(100, timeoutMs - static_cast<int>(timer.elapsed())));
            QCoreApplication::processEvents(QEventLoop::AllEvents, waitChunkMs);
            if (m_socket.bytesAvailable() == 0 && !m_socket.waitForReadyRead(waitChunkMs)) {
                continue;
            }

            m_buffer.append(m_socket.readAll());
            while (true) {
                QJsonObject packet;
                const Protocol::DecodeStatus status = Protocol::tryDecode(m_buffer, packet);
                if (status == Protocol::DecodeStatus::NeedMoreData) {
                    break;
                }

                if (status != Protocol::DecodeStatus::Ok) {
                    continue;
                }

                const QString type = packet.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("ping")) {
                    send({{QStringLiteral("type"), QStringLiteral("pong")}});
                    continue;
                }

                if (type == QStringLiteral("ack") || type == QStringLiteral("pong")) {
                    continue;
                }

                const quint32 seq = static_cast<quint32>(packet.value(QStringLiteral("seq")).toInt());
                const QString id = packet.value(QStringLiteral("id")).toString();
                if (seq > 0 && !id.isEmpty()) {
                    send({
                        {QStringLiteral("type"), QStringLiteral("ack")},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("seq"), static_cast<qint64>(seq)}
                    });
                }

                m_packets.append(packet);
            }
        }

        return false;
    }

    QTcpSocket m_socket;
    QByteArray m_buffer;
    QList<QJsonObject> m_packets;
    quint32 m_nextOutgoingSeq {0};
};

}  // namespace

class ServerIntegrationTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registerResumeAndLogoutFlow();
    void offlinePrivateDeliveryAndReadFlow();
    void loginRateLimitBlocksRepeatedAttempts();
};

void ServerIntegrationTests::initTestCase()
{
    QString probeError;
    QVERIFY2(TestSupport::probeSqliteDriver(&probeError), qPrintable(probeError));
}

void ServerIntegrationTests::registerResumeAndLogoutFlow()
{
    Server server;
    server.setDatabasePath(TestSupport::sharedMemoryDatabaseUri(QStringLiteral("integration_auth")));
    QVERIFY(server.start(0));
    QVERIFY(server.listeningPort() > 0);

    TestSocketClient firstClient;
    QVERIFY(firstClient.connectTo(server.listeningPort()));

    QVERIFY(firstClient.send({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeRegister)},
        {AuthProtocol::kFieldUsername, QStringLiteral("alice")},
        {AuthProtocol::kFieldPassword, QStringLiteral("supersecret")}
    }));

    QJsonObject authOkPacket;
    QVERIFY(firstClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthOk), &authOkPacket));

    QString username;
    AuthProtocol::SessionInfo firstSession;
    QVERIFY(AuthProtocol::parseAuthOkPacket(authOkPacket, username, firstSession));
    QCOMPARE(username, QStringLiteral("alice"));
    QVERIFY(firstSession.isValid());

    firstClient.disconnect();

    TestSocketClient resumedClient;
    QVERIFY(resumedClient.connectTo(server.listeningPort()));
    QVERIFY(resumedClient.send(AuthProtocol::makeResumeSessionPacket(QStringLiteral("alice"), firstSession.token)));

    QJsonObject resumedAuthPacket;
    QVERIFY(resumedClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthOk), &resumedAuthPacket));

    AuthProtocol::SessionInfo resumedSession;
    QVERIFY(AuthProtocol::parseAuthOkPacket(resumedAuthPacket, username, resumedSession));
    QCOMPARE(username, QStringLiteral("alice"));
    QVERIFY(resumedSession.isValid());
    QVERIFY(resumedSession.token != firstSession.token);

    QVERIFY(resumedClient.send(AuthProtocol::makeLogoutPacket()));
    QVERIFY(resumedClient.waitForDisconnected());

    TestSocketClient staleClient;
    QVERIFY(staleClient.connectTo(server.listeningPort()));
    QVERIFY(staleClient.send(AuthProtocol::makeResumeSessionPacket(QStringLiteral("alice"), resumedSession.token)));

    QJsonObject invalidPacket;
    QVERIFY(staleClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeSessionInvalid), &invalidPacket));
    QVERIFY(!invalidPacket.value(AuthProtocol::kFieldMessage).toString().isEmpty());
}

void ServerIntegrationTests::offlinePrivateDeliveryAndReadFlow()
{
    Server server;
    server.setDatabasePath(TestSupport::sharedMemoryDatabaseUri(QStringLiteral("integration_private")));
    QVERIFY(server.start(0));
    QVERIFY(server.listeningPort() > 0);

    TestSocketClient aliceClient;
    TestSocketClient bobClient;
    QVERIFY(aliceClient.connectTo(server.listeningPort()));
    QVERIFY(bobClient.connectTo(server.listeningPort()));

    QVERIFY(aliceClient.send({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeRegister)},
        {AuthProtocol::kFieldUsername, QStringLiteral("alice")},
        {AuthProtocol::kFieldPassword, QStringLiteral("supersecret")}
    }));
    QVERIFY(bobClient.send({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeRegister)},
        {AuthProtocol::kFieldUsername, QStringLiteral("bob")},
        {AuthProtocol::kFieldPassword, QStringLiteral("supersecret")}
    }));

    QJsonObject packet;
    QVERIFY(aliceClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthOk), &packet));
    QVERIFY(bobClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthOk), &packet));

    bobClient.disconnect();

    const QString messageId = aliceClient.sendReliable({
        {QStringLiteral("type"), QStringLiteral("private")},
        {QStringLiteral("to"), QStringLiteral("bob")},
        {QStringLiteral("text"), QStringLiteral("queued hello")}
    });

    QJsonObject queuedPacket;
    QVERIFY(aliceClient.waitForPacketType(QStringLiteral("queued"), &queuedPacket));
    QCOMPARE(queuedPacket.value(QStringLiteral("id")).toString(), messageId);
    QCOMPARE(queuedPacket.value(QStringLiteral("to")).toString(), QStringLiteral("bob"));

    TestSocketClient bobLoginClient;
    QVERIFY(bobLoginClient.connectTo(server.listeningPort()));
    QVERIFY(bobLoginClient.send({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeLogin)},
        {AuthProtocol::kFieldUsername, QStringLiteral("bob")},
        {AuthProtocol::kFieldPassword, QStringLiteral("supersecret")}
    }));

    QJsonObject bobAuthPacket;
    QVERIFY(bobLoginClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthOk), &bobAuthPacket));

    QJsonObject privatePacket;
    QVERIFY(bobLoginClient.waitForPacketType(QStringLiteral("private"), &privatePacket));
    QCOMPARE(privatePacket.value(QStringLiteral("id")).toString(), messageId);
    QCOMPARE(privatePacket.value(QStringLiteral("from")).toString(), QStringLiteral("alice"));
    QCOMPARE(privatePacket.value(QStringLiteral("to")).toString(), QStringLiteral("bob"));
    QCOMPARE(privatePacket.value(QStringLiteral("text")).toString(), QStringLiteral("queued hello"));

    QJsonObject deliveredPacket;
    QVERIFY(aliceClient.waitForPacketType(QStringLiteral("delivered"), &deliveredPacket));
    QCOMPARE(deliveredPacket.value(QStringLiteral("id")).toString(), messageId);
    QCOMPARE(deliveredPacket.value(QStringLiteral("to")).toString(), QStringLiteral("bob"));

    QVERIFY(bobLoginClient.send({
        {QStringLiteral("type"), QStringLiteral("read")},
        {QStringLiteral("to"), QStringLiteral("alice")},
        {QStringLiteral("id"), messageId}
    }));

    QJsonObject readPacket;
    QVERIFY(aliceClient.waitForPacketType(QStringLiteral("read"), &readPacket));
    QCOMPARE(readPacket.value(QStringLiteral("id")).toString(), messageId);
    QCOMPARE(readPacket.value(QStringLiteral("from")).toString(), QStringLiteral("bob"));
}

void ServerIntegrationTests::loginRateLimitBlocksRepeatedAttempts()
{
    Server server;
    server.setDatabasePath(TestSupport::sharedMemoryDatabaseUri(QStringLiteral("integration_rate_limit")));
    QVERIFY(server.start(0));
    QVERIFY(server.listeningPort() > 0);

    TestSocketClient bootstrapClient;
    QVERIFY(bootstrapClient.connectTo(server.listeningPort()));
    QVERIFY(bootstrapClient.send({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeRegister)},
        {AuthProtocol::kFieldUsername, QStringLiteral("rate_user")},
        {AuthProtocol::kFieldPassword, QStringLiteral("supersecret")}
    }));

    QJsonObject packet;
    QVERIFY(bootstrapClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthOk), &packet));
    bootstrapClient.disconnect();

    TestSocketClient attackerClient;
    QVERIFY(attackerClient.connectTo(server.listeningPort()));

    for (int attempt = 0; attempt < 6; ++attempt) {
        QVERIFY(attackerClient.send({
            {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeLogin)},
            {AuthProtocol::kFieldUsername, QStringLiteral("rate_user")},
            {AuthProtocol::kFieldPassword, QStringLiteral("wrong-pass")}
        }));

        QJsonObject errorPacket;
        QVERIFY(attackerClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthError), &errorPacket));
        QVERIFY(!errorPacket.value(AuthProtocol::kFieldMessage).toString().isEmpty());
    }

    QVERIFY(attackerClient.send({
        {AuthProtocol::kFieldType, QString::fromLatin1(AuthProtocol::kTypeLogin)},
        {AuthProtocol::kFieldUsername, QStringLiteral("rate_user")},
        {AuthProtocol::kFieldPassword, QStringLiteral("wrong-pass")}
    }));

    QJsonObject limitedPacket;
    QVERIFY(attackerClient.waitForPacketType(QString::fromLatin1(AuthProtocol::kTypeAuthError), &limitedPacket));
    QVERIFY(limitedPacket.value(AuthProtocol::kFieldMessage).toString().contains(QStringLiteral("Too many login attempts")));
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("server_integration_tests"));
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
    ServerIntegrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "server_integration_tests.moc"
