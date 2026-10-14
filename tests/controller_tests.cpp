#include "trading/controller.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QtTest>

using namespace grid;

class ControllerTests : public QObject {
    Q_OBJECT
private slots:

    void batchOrdersProtectInventoryAndCommitOnce()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("batch.db");
        Store store(path);
        Engine e;
        e.profileId = "batch";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.shorts = {80000 * Scale, 90000 * Scale, 850 * Dollars, 1};
        e.quote(82500 * Scale, 82500 * Scale);
        e.requestRebuild(e.config);
        Client client;
        TradeController controller(e, store, client);
        const auto json = [&](const Action& action, int filled, bool terminal, int timestamp) {
            return QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_type", "limit"},
                {"direction", action.buy ? "buy" : "sell"},
                {"label", QString::fromStdString(action.label)},
                {"order_id", QString::fromStdString("id-" + action.label)},
                {"amount", double(action.total) / 10000}, {"filled_amount", double(filled) / 10000},
                {"price", double(action.price) / Scale},
                {"order_state", terminal ? "filled" : "open"},
                {"last_update_timestamp", timestamp}};
        };
        const auto entry = e.nextAction();
        QVERIFY(entry);
        QVERIFY(controller.receiveOwnOrder(json(*entry, 1, false, 1), true, false));
        const auto take = e.nextAction();
        QVERIFY(take);
        QCOMPARE(take->role, Role::Take);
        QVERIFY(controller.receiveOwnOrder(json(*take, 0, false, 2), true, false));
        Engine observer;
        QVERIFY(!store.load(observer));
        const auto notifications = controller.orderedNotifications(
            QJsonArray{json(*take, 1, true, 3), json(*entry, 2, false, 3)});
        QCOMPARE(notifications.size(), size_t(2));
        QCOMPARE(notifications.front().label, entry->label);
        for (const auto& notification : notifications) {
            QVERIFY(notification.own);
            QVERIFY(
                controller.receiveOwnOrder(notification.json, false, false, &notification.label));
        }
        QCOMPARE(e.cycle(e.order(entry->label)->cycle)->held, Int(1));
        QVERIFY(!store.load(observer));
        store.save(e);
        QVERIFY(store.load(observer));
        QCOMPARE(observer.cycles()[0].held, Int(1));
        const auto protection = e.nextAction();
        QVERIFY(protection);
        QCOMPARE(protection->role, Role::Take);
        QCOMPARE(protection->total, Int(1));
    }

    void malformedSuccessRequiresReconciliation_data()
    {
        QTest::addColumn<bool>("missing");
        QTest::newRow("wrong-label") << false;
        QTest::newRow("missing-order") << true;
    }

    void malformedSuccessRequiresReconciliation()
    {
        QFETCH(bool, missing);
        QTemporaryDir dir;
        const auto path = dir.filePath("request.db");
        Store store(path);
        Engine e;
        e.profileId = "controller";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.shorts = {80000 * Scale, 90000 * Scale, 850 * Dollars, 1};
        e.quote(82500 * Scale, 82500 * Scale);
        e.requestRebuild(e.config);
        const auto action = e.nextAction();
        QVERIFY(action);
        QWebSocketServer server("Request simulator", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        bool durable = false;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto request = QJsonDocument::fromJson(frame.toUtf8()).object();
                QJsonValue result = QJsonObject{{"ok", true}};
                if (request["method"] == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                if (request["method"] == "private/sell") {
                    Engine restored;
                    Store observer(path);
                    durable = observer.load(restored) && restored.order(action->label) &&
                              restored.order(action->label)->mutation == Mutation::Create;
                    result = missing ? QJsonObject{}
                                     : QJsonObject{{"order_id", "unexpected-id"},
                                           {"label", "another-profile"}};
                }
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(
                    QJsonObject{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}})
                        .toJson()));
            });
        });
        Client client;
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        client.connectTo("fake", "fake-secret", false,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        TradeController controller(e, store, client);
        bool finished = false;
        RpcResult response;
        controller.execute(*action, [&](const RpcResult& r) {
            response = r;
            finished = true;
        });
        QTRY_VERIFY(finished);
        QVERIFY(durable);
        QVERIFY(!response.ok);
        QVERIFY(response.uncertain);
        e.uncertain(action->label);
        QVERIFY(!e.nextAction());
        auto foreign = *action;
        foreign.label = "other-profile";
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error, controller.execute(foreign, [](const RpcResult&) {
            }));
        client.stop();
    }
};
QTEST_GUILESS_MAIN(ControllerTests)
#include "controller_tests.moc"
