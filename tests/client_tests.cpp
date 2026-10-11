#include "client.h"
#include <QJsonDocument>
#include <QWebSocketServer>
#include <QtTest>

class ClientTests : public QObject {
    Q_OBJECT
private slots:
    void destructionWithPendingRequests() {
        QWebSocketServer server("Destruction simulator", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QWebSocket* peer = nullptr;
        connect(&server, &QWebSocketServer::newConnection, this, [&] { peer = server.nextPendingConnection(); peer->setParent(&server); });
        bool callback = false;
        {
            Client client;
            client.connectTo("fake", "fake-secret", false, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
            QTRY_VERIFY(peer);
            client.call("pending/read", {}, [&](const RpcResult&) { callback = true; });
            // Deliberately destroy without an explicit stop and with a live socket/auth callback.
        }
        QTest::qWait(50); QVERIFY(!callback);
    }
    void correlationHeartbeatAndLostMutation() {
        QWebSocketServer server("Fake Deribit", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Client client; QSignalSpy authenticated(&client, &Client::authenticatedSession);
        QSignalSpy events(&client, &Client::subscription); QSignalSpy notices(&client, &Client::notice);
        QWebSocket* peer = nullptr; QList<QJsonObject> requests; bool authParametersOK = false;
        auto reply = [](QWebSocket* socket, qint64 id, const QJsonValue& result) {
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            peer = server.nextPendingConnection(); peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&](const QString& frame) {
                const auto o = QJsonDocument::fromJson(frame.toUtf8()).object(); const auto method = o["method"].toString(); const qint64 id = o["id"].toInteger();
                if (method == "public/auth") {
                    const auto p = o["params"].toObject(); authParametersOK = p["client_id"] == "test-id" && p["client_secret"] == "test-secret" && p["grant_type"] == "client_credentials";
                    reply(peer, id, QJsonObject{{"expires_in", 3600}, {"refresh_token", "test-refresh"}});
                } else if (method == "public/set_heartbeat" || method == "public/test") reply(peer, id, QJsonObject{{"ok", true}});
                else requests.append(o);
            });
        });
        client.connectTo("test-id", "test-secret", false, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1); QVERIFY(authParametersOK);
        int first = 0, second = 0;
        const auto a = client.call("read/first", {}, [&](const RpcResult& r) { if (r.ok) first = r.value.toInt(); });
        const auto b = client.call("read/second", {}, [&](const RpcResult& r) { if (r.ok) second = r.value.toInt(); });
        QTRY_COMPARE(requests.size(), 2); reply(peer, b, 22); reply(peer, a, 11);
        QTRY_COMPARE(first, 11); QTRY_COMPARE(second, 22);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"method", "subscription"}, {"params", QJsonObject{{"channel", "ticker.test.100ms"}, {"data", QJsonObject{{"price", 1}}}}}}).toJson()));
        QTRY_COMPARE(events.size(), 1);
        int calls = 0; bool uncertain = false;
        client.call("private/buy", {{"type", "limit"}}, [&](const RpcResult& r) { ++calls; uncertain = r.uncertain; }, true);
        QTRY_COMPARE(requests.size(), 3); peer->close(); QTRY_COMPARE(calls, 1); QVERIFY(uncertain);
        client.stop();
        for (const auto& row : notices) QVERIFY(!row[0].toString().contains("test-secret"));
    }
};
QTEST_GUILESS_MAIN(ClientTests)
#include "client_tests.moc"
