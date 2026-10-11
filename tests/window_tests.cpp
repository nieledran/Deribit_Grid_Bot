#include "window.h"
#include <QJsonDocument>
#include <QDateTime>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QMessageBox>
#include <QtTest>
#include <stdexcept>

using namespace grid;
class WindowTests : public QObject {
    Q_OBJECT
private slots:
    void manualAndExistingBotTakesSurviveRestart_data() {
        QTest::addColumn<bool>("trigger");
        QTest::newRow("manual-limit") << false;
        QTest::newRow("manual-trigger-market") << true;
    }
    void manualAndExistingBotTakesSurviveRestart() {
        QFETCH(bool, trigger);
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("takes.db");
        Engine seed; seed.profileId = "preservetakes"; seed.config.instrument = "ETH_USDC-PERPETUAL";
        seed.spec = {Scale / 100, Scale / 10000, {}}; seed.externalPosition = 4000000;
        seed.config.longs = {2480 * Scale, 2500 * Scale, 100 * Dollars, 2}; seed.config.shorts = {2500 * Scale, 2520 * Scale, 100 * Dollars, 2};
        seed.importCycle({seed.nextCycle++, 7, Side::Short, 2506 * Scale, 2504 * Scale, 40, 40, true});
        Order saved; saved.label = "dg_preservetakes_1_t_1"; saved.exchangeId = "existing-bot-take"; saved.cycle = 1;
        saved.role = Role::Take; saved.price = 2504 * Scale; saved.total = 40; saved.status = Status::Open; seed.importOrder(saved); seed.nextOrder = 2;
        { Store db(path); db.save(seed); }
        const QJsonObject manual{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"order_id", "USDC-210049237502281438"}, {"label", "manual"},
            {"order_type", trigger ? "take_market" : "limit"}, {"order_state", trigger ? "untriggered" : "open"}, {"direction", "sell"},
            {"price", trigger ? QJsonValue("market_price") : QJsonValue(2510.0)}, {"reduce_only", true}, {"amount", 0.04}, {"filled_amount", 0.0}};
        const QJsonObject take{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"order_id", "existing-bot-take"}, {"label", QString::fromStdString(saved.label)},
            {"order_type", "limit"}, {"order_state", "open"}, {"direction", "buy"}, {"price", 2504.0}, {"amount", 0.004}, {"filled_amount", 0.0}, {"last_update_timestamp", 1}};
        QHash<QString, QJsonObject> entries; QStringList cancelled, edited; int takeCreates = 0;
        QWebSocketServer server("Preserve takes", QWebSocketServer::NonSecureMode); QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection(); peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object(); const auto method = j["method"].toString(); const auto p = j["params"].toObject();
                QJsonValue result = QJsonObject{{"ok", true}};
                if (method == "public/auth") result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary") result = QJsonObject{{"id", 7654321}};
                else if (method == "public/get_instrument") result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"base_currency", "ETH"}, {"settlement_currency", "USDC"}, {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"}, {"tick_size", 0.01}, {"min_trade_amount", 0.0001}, {"contract_size", 0.0001}};
                else if (method == "private/get_position") result = QJsonObject{{"size_currency", 0.036}};
                else if (method == "public/ticker") result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"best_bid_price", 2505.07}, {"best_ask_price", 2505.08}};
                else if (method == "private/get_open_orders_by_instrument") {
                    QJsonArray open{manual, take}; for (const auto& o : entries) if (o["order_state"] == "open") open.append(o); result = open;
                } else if (method == "private/get_order_state") result = p["order_id"] == "existing-bot-take" ? take : entries.value(p["order_id"].toString());
                else if (method == "private/buy" || method == "private/sell") {
                    if (p["label"].toString().contains("_t_")) ++takeCreates;
                    const QString id = "entry-" + p["label"].toString();
                    const QJsonObject o{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"order_id", id}, {"label", p["label"]}, {"order_type", "limit"},
                        {"order_state", "open"}, {"direction", method == "private/buy" ? "buy" : "sell"}, {"price", p["price"]}, {"amount", p["amount"]}, {"filled_amount", 0.0}, {"last_update_timestamp", 1}};
                    entries[id] = o; result = QJsonObject{{"order", o}};
                } else if (method == "private/cancel") {
                    const QString id = p["order_id"].toString(); cancelled.append(id);
                    auto o = entries.value(id); o["order_state"] = "cancelled"; o["last_update_timestamp"] = 2; entries[id] = o; result = o;
                } else if (method == "private/edit") { edited.append(p["order_id"].toString()); result = QJsonObject{{"order", take}}; }
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", j["id"]}, {"result", result}}).toJson()));
            });
        });
        for (int run = 0; run < 2; ++run) {
            auto window = std::make_unique<Window>(path, "ETH_USDC-PERPETUAL", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
            window->findChild<QLineEdit*>("clientId")->setText("fake"); window->findChild<QLineEdit*>("clientSecret")->setText("secret"); window->findChild<QPushButton*>("connect")->click();
            QTRY_COMPARE_WITH_TIMEOUT(window->findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
            QTRY_VERIFY(window->findChild<QPushButton*>("start")->isEnabled()); window->findChild<QPushButton*>("start")->click();
            if (!trigger) QTRY_VERIFY(entries.size() >= 2 * (run + 1)); else QTest::qWait(850);
            if (trigger) QVERIFY(entries.isEmpty());
            for (const auto& o : entries) QCOMPARE(o["direction"].toString(), QString("buy"));
            window->findChild<QPushButton*>("pause")->click();
            QTRY_VERIFY_WITH_TIMEOUT(([&] { Store db(path); Engine e; db.load(e); return e.settled() && !e.running; })(), 5000);
            Engine restored; { Store db(path); QVERIFY(db.load(restored)); }
            const auto* restoredTake = restored.order(saved.label); QVERIFY(restoredTake); QVERIFY(restoredTake->live());
            QCOMPARE(restoredTake->exchangeId, saved.exchangeId); QCOMPARE(restoredTake->price, saved.price); QCOMPARE(restoredTake->total, saved.total);
            QCOMPARE(restored.foreign.size(), size_t(0)); // Foreign orders are read from the exchange, never adopted into saved cycles.
            QCOMPARE(takeCreates, 0); QVERIFY(edited.isEmpty());
            QVERIFY(!cancelled.contains("existing-bot-take")); QVERIFY(!cancelled.contains("USDC-210049237502281438"));
            window->close(); QTest::qWait(500); window.reset();
        }
    }
    void restartAfterManualFlattenDoesNotRestoreOldTakes() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("old.db");
        Engine seed; seed.profileId = "oldeth"; seed.config.instrument = "ETH_USDC-PERPETUAL";
        seed.spec = {Scale / 100, Scale / 10000, {}};
        seed.config.longs = {2480 * Scale, 2500 * Scale, 1000 * Dollars, 10};
        seed.config.shorts = {2500 * Scale, 2520 * Scale, 1000 * Dollars, 10};
        for (int i = 0; i < 3; ++i) seed.importCycle({seed.nextCycle++, 7, Side::Long, 2500 * Scale - (i + 1) * Scale / 5, 2500 * Scale - i * Scale / 5, 40, 40, true});
        { Store db(path); db.save(seed); }
        QWebSocketServer server("Manual flatten", QWebSocketServer::NonSecureMode); QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int tradingCalls = 0;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection(); peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object(); const auto method = j["method"].toString();
                QJsonValue result = QJsonObject{{"ok", true}};
                if (method == "public/auth") result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary") result = QJsonObject{{"id", 87654321}};
                else if (method == "public/get_instrument") result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"base_currency", "ETH"}, {"settlement_currency", "USDC"}, {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"}, {"tick_size", 0.01}, {"min_trade_amount", 0.0001}, {"contract_size", 0.0001}};
                else if (method == "private/get_open_orders_by_instrument") result = QJsonArray{};
                else if (method == "private/get_position") result = QJsonObject{{"size_currency", 0.0}};
                else if (method == "public/ticker") result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"best_bid_price", 2500.19}, {"best_ask_price", 2500.39}};
                else if (method == "private/buy" || method == "private/sell" || method == "private/edit") ++tradingCalls;
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", j["id"]}, {"result", result}}).toJson()));
            });
        });
        Window window(path, "ETH_USDC-PERPETUAL", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        window.findChild<QLineEdit*>("clientId")->setText("fake"); window.findChild<QLineEdit*>("clientSecret")->setText("secret");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
        QTest::qWait(850); QCOMPARE(tradingCalls, 0); QVERIFY(!window.findChild<QPushButton*>("start")->isEnabled());
        Engine restored; { Store db(path); QVERIFY(db.load(restored)); }
        QVERIFY(restored.positionBlocked); QCOMPARE(restored.trackedPosition(), Int(1200000));
        // Only the explicitly confirmed reset discards inventory that was closed manually.
        QVERIFY(window.findChild<QPushButton*>("resetHistory")->isEnabled());
        QTimer::singleShot(0, [] { if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click(); });
        window.findChild<QPushButton*>("resetHistory")->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] { Store db(path); Engine cleared; db.load(cleared); return cleared.cycles().empty() && !cleared.positionBlocked; })(), 5000);
        QCOMPARE(tradingCalls, 0); window.close();
    }
    void pauseProtectsAFillRacingCancellation_data() {
        QTest::addColumn<bool>("recover");
        QTest::addColumn<bool>("isLong");
        QTest::newRow("live-partial-short") << false << false;
        QTest::newRow("accepted-short-with-lost-reply") << true << false;
        QTest::newRow("live-partial-long") << false << true;
        QTest::newRow("accepted-long-with-lost-reply") << true << true;
    }
    void pauseProtectsAFillRacingCancellation() {
        QFETCH(bool, recover);
        QFETCH(bool, isLong);
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("profile.db");
        Engine seed; seed.profileId = "windowtest";
        if (isLong) seed.config.longs = {80000 * Scale, 84500 * Scale, 850 * Dollars, 1};
        else seed.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 1};
        if (recover) {
            seed.spec = {Scale / 10, Scale / 10000, {}};
            seed.quote(84600 * Scale - Scale / 10, 84600 * Scale + Scale / 10); seed.requestRebuild(seed.config);
            QVERIFY(seed.nextAction());
        }
        { Store db(path); db.save(seed); }
        QWebSocketServer server("Fake Deribit", QWebSocketServer::NonSecureMode); QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QJsonObject entry, take; int entries = 0, takes = 0, cancellations = 0, edits = 0; bool limitsOnly = true;
        QJsonArray entryFees, takeFees;
        const auto trade = [](const QString& order, const QString& id, double quantity, double fee) {
            return QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_id", order}, {"trade_id", id},
                {"timestamp", QDateTime::currentMSecsSinceEpoch()}, {"amount", quantity}, {"fee", fee}, {"fee_currency", "USDC"}};
        };
        QWebSocket* peer = nullptr;
        if (recover) {
            const auto& o = seed.orders()[0];
            entry = QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_type", "limit"}, {"order_state", "open"}, {"direction", isLong ? "buy" : "sell"},
                {"label", QString::fromStdString(o.label)}, {"order_id", "entry-1"}, {"price", static_cast<double>(o.price) / Scale},
                {"amount", static_cast<double>(o.total) / 10000}, {"filled_amount", 0.0003}, {"last_update_timestamp", 1}};
            auto historical = trade("entry-1", "old-entry", 0.0003, 123.0); historical["timestamp"] = 1; entryFees.append(historical);
        }
        const auto response = [](QWebSocket* socket, qint64 id, const QJsonValue& result) {
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).toJson()));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection(); socket->setParent(&server);
            peer = socket;
            connect(socket, &QWebSocket::textMessageReceived, this, [&, socket](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object(); const auto method = j["method"].toString(); const auto p = j["params"].toObject(); const auto id = j["id"].toInteger();
                if (method == "public/auth") response(socket, id, QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}});
                else if (method == "public/get_instrument") response(socket, id, QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"base_currency", "BTC"}, {"settlement_currency", "USDC"}, {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"}, {"tick_size", 0.1}, {"min_trade_amount", 0.0001}, {"contract_size", 0.0001}});
                else if (method == "private/get_account_summary") response(socket, id, QJsonObject{{"id", 987654321}});
                else if (method == "private/get_position") response(socket, id, QJsonObject{{"size_currency", (recover ? 0.0 : -0.0004) + (isLong ? 1 : -1) * (entry["filled_amount"].toDouble() - take["filled_amount"].toDouble())}});
                else if (method == "public/ticker") response(socket, id, QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"best_bid_price", 84599.9}, {"best_ask_price", 84600.1}});
                else if (method == "private/get_open_orders_by_instrument") response(socket, id, QJsonArray{});
                else if (method == "private/get_order_state_by_label") response(socket, id, QJsonArray{entry});
                else if (method == "private/get_order_state") response(socket, id, p["order_id"] == "take-1" ? take : entry);
                else if (method == "private/get_user_trades_by_order") response(socket, id, p["order_id"] == "take-1" ? takeFees : entryFees);
                else if (method == "private/buy" || method == "private/sell") {
                    limitsOnly = limitsOnly && p["type"] == "limit" && p["post_only"] == false && p["time_in_force"] == "good_til_cancelled";
                    const bool buy = method == "private/buy"; const bool isTake = buy != isLong; auto& o = isTake ? take : entry;
                    o = QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_type", "limit"}, {"order_state", "open"}, {"direction", buy ? "buy" : "sell"},
                        {"label", p["label"]}, {"order_id", isTake ? "take-1" : "entry-1"}, {"price", p["price"]}, {"amount", p["amount"]}, {"filled_amount", isTake ? 0.0 : 0.0003}, {"last_update_timestamp", 1}};
                    if (isTake) ++takes; else { ++entries; entryFees.append(trade("entry-1", "entry-a", 0.0003, 0.01)); }
                    response(socket, id, QJsonObject{{"order", o}, {"trades", isTake ? QJsonArray{} : entryFees}});
                } else if (method == "private/cancel") {
                    ++cancellations; entry["order_state"] = "cancelled"; entry["filled_amount"] = 0.0004; entry["last_update_timestamp"] = 2;
                    entryFees.append(trade("entry-1", "entry-b", 0.0001, 0.003)); response(socket, id, entry);
                } else if (method == "private/edit") {
                    ++edits; take["amount"] = p["amount"]; take["last_update_timestamp"] = 3; response(socket, id, QJsonObject{{"order", take}});
                } else response(socket, id, QJsonObject{{"ok", true}});
            });
        });
        Window window(path, "BTC_USDC-PERPETUAL", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Window(path, "BTC_USDC-PERPETUAL"));
        window.findChild<QLineEdit*>("clientId")->setText("fake-id"); window.findChild<QLineEdit*>("clientSecret")->setText("fake-secret");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
        if (!recover) {
            QTRY_VERIFY(window.findChild<QPushButton*>("start")->isEnabled()); window.findChild<QPushButton*>("start")->click();
            QTRY_COMPARE_WITH_TIMEOUT(takes, 1, 5000); QCOMPARE(entries, 1);
            window.findChild<QPushButton*>("pause")->click();
            QTRY_COMPARE_WITH_TIMEOUT(edits, 1, 5000);
        } else { QTRY_COMPARE_WITH_TIMEOUT(takes, 1, 5000); QTRY_COMPARE_WITH_TIMEOUT(edits, 1, 5000); }
        QVERIFY(limitsOnly); QCOMPARE(cancellations, 1); QCOMPARE(take["amount"].toDouble(), 0.0004);
        QTest::qWait(100);
        Engine restored; { Store db(path); QVERIFY(db.load(restored)); }
        QCOMPARE(restored.cycles()[0].held, Int(4)); QVERIFY(restored.settled()); QVERIFY(!restored.running);
        QCOMPARE(takes, 1); QCOMPARE(entries, recover ? 0 : 1);
        auto* stats = window.findChild<QLabel*>("sessionStats"); QVERIFY(stats);
        QVERIFY(!window.findChild<QPushButton*>("resetHistory")->isEnabled());
        const QString entryCost = recover ? "0.0030" : "0.0130";
        QTRY_COMPARE(stats->text(), QString("Approx. P&L: 0.0000 USDC\nFees (entry + take): %1 USDC\nClosed trades: 0").arg(entryCost));
        const auto publish = [&](const QJsonObject& order, const QJsonArray& trades = QJsonArray{}) {
            const QJsonObject data{{"orders", order.isEmpty() ? QJsonArray{} : QJsonArray{order}}, {"trades", trades}};
            const QJsonObject params{{"channel", "user.changes.BTC_USDC-PERPETUAL.100ms"}, {"data", data}};
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"method", "subscription"}, {"params", params}}).toJson()));
        };
        // Response/subscription duplicates and manual trades do not add fees again.
        auto manual = trade("manual-order", "manual", 0.0001, 42.0);
        auto duplicateFees = entryFees; duplicateFees.append(manual); publish({}, duplicateFees);
        take["filled_amount"] = 0.0001; take["last_update_timestamp"] = 4;
        takeFees.append(trade("take-1", "take-a", 0.0001, -0.001));
        const auto partial = take; publish(partial, takeFees); publish(partial, takeFees);
        const QString partialPnl = isLong ? "0.4500" : "0.5400";
        const QString partialCost = recover ? "0.0020" : "0.0120";
        QTRY_COMPARE(stats->text(), QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 0").arg(partialPnl, partialCost));
        take["filled_amount"] = 0.0004; take["last_update_timestamp"] = 5; take["order_state"] = "filled";
        auto finalTrade = trade("take-1", "take-b", 0.0003, 0.006);
        auto missingFee = finalTrade; missingFee.remove("fee"); takeFees.append(missingFee);
        publish(take, takeFees); publish(take, takeFees); publish(partial);
        const QString fullPnl = isLong ? "1.8000" : "2.1600";
        QTRY_COMPARE(stats->text(), QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC (incomplete)\nClosed trades: 1").arg(fullPnl, partialCost));
        auto wrongCurrency = finalTrade; wrongCurrency["fee_currency"] = "BTC"; publish({}, QJsonArray{wrongCurrency});
        QTest::qWait(350); QVERIFY(stats->text().contains("(incomplete)"));
        // A reconnect obtains the missed fee once, including for an already closed order.
        takeFees.replace(1, finalTrade);
        window.findChild<QPushButton*>("connect")->click();
        const QString fullCost = recover ? "0.0080" : "0.0180";
        QTRY_COMPARE_WITH_TIMEOUT(stats->text(), QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 1").arg(fullPnl, fullCost), 5000);
        publish({}, takeFees); publish(partial, entryFees);
        QTest::qWait(100);
        QCOMPARE(stats->text(), QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 1").arg(fullPnl, fullCost));
        // Clear log leaves the durable inventory and counters intact.
        window.findChild<QPushButton*>("clearLog")->click(); QVERIFY(window.findChild<QPlainTextEdit*>()->toPlainText().isEmpty());
        const auto nextOrder = restored.nextOrder;
        QTRY_VERIFY(window.findChild<QPushButton*>("resetHistory")->isEnabled());
        QTimer::singleShot(0, [] { if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click(); });
        window.findChild<QPushButton*>("resetHistory")->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] { Store db(path); Engine cleared; db.load(cleared); return cleared.cycles().empty() && cleared.orders().empty(); })(), 5000);
        { Store db(path); QVERIFY(db.load(restored)); }
        QVERIFY(restored.externalPosition.has_value()); QCOMPARE(*restored.externalPosition, recover ? Int(0) : Int(-40000));
        QVERIFY(restored.nextOrder >= nextOrder); QVERIFY(!restored.running);
        QCOMPARE(stats->text(), QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 1").arg(fullPnl, fullCost));
        window.close();
        // Session counters are deliberately absent from the saved state.
        { Store db(path); QVERIFY(db.load(restored)); }
        QVERIFY(!encodeEngine(restored).contains("sessionPnl"));
        QVERIFY(!encodeEngine(restored).contains("sessionFees"));
    }
};
QTEST_MAIN(WindowTests)
#include "window_tests.moc"
