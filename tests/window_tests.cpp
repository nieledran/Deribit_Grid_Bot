#include "ui/window.h"
#include <QJsonDocument>
#include <QDateTime>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QMessageBox>
#include <QSpinBox>
#include <QtTest>
#include <stdexcept>
#include <algorithm>

using namespace grid;

class WindowTests : public QObject {
    Q_OBJECT
private slots:

    void profileCancellationUsesCurrentDistance()
    {
        QTemporaryDir dir;
        Store store(dir.filePath("cancel-distance.db"));
        Engine engine;
        engine.profileId = "distance";
        engine.quote(1000 * Scale, 1000 * Scale);
        Client client;
        int session = 0;
        ProfileController profiles(engine, store, client, session,
            std::make_unique<QLockFile>(dir.filePath("profile.lock")), this);
        QSignalSpy authenticated(&client, &Client::authenticatedSession);
        QSignalSpy completed(&profiles, &ProfileController::cancelled);
        QSignalSpy failed(&profiles, &ProfileController::failed);
        QMap<QString, QJsonValue> prices{{"dg_distance_1_e_1", 990.0},
            {"dg_distance_2_e_2", 900.0}, {"dg_distance_10_e_10", 1015.0},
            {"dg_distance_3_e_3", "market"}};
        QStringList cancelled;
        QWebSocketServer server("Cancellation distance", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = j["method"].toString();
                QJsonValue result = QJsonObject{{"expires_in", 3600}};
                if (method == "private/get_open_orders_by_instrument") {
                    QJsonArray orders;
                    for (auto it = prices.cbegin(); it != prices.cend(); ++it)
                        orders.append(QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                            {"order_id", "ex-" + it.key()}, {"label", it.key()}, {"price", it.value()}});
                    result = orders;
                } else if (method == "private/cancel_by_label") {
                    const auto label = j["params"].toObject()["label"].toString();
                    cancelled.append(label);
                    result = prices.remove(label);
                    // The remaining nearer order switches when the current quote changes.
                    engine.quote(905 * Scale, 905 * Scale);
                }
                peer->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{"id", j["id"]}, {"result", result}}).toJson()));
            });
        });
        client.connectTo("fake", "fake", false,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QTRY_COMPARE(authenticated.size(), 1);
        profiles.cancelOrders(false, session);
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 7000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(cancelled, (QStringList{"dg_distance_1_e_1", "dg_distance_2_e_2",
                                "dg_distance_10_e_10", "dg_distance_3_e_3"}));
        client.stop();
    }

    void applyRebuildsOnlyChangedSide_data()
    {
        QTest::addColumn<bool>("eth");
        QTest::newRow("BTC") << false;
        QTest::newRow("ETH") << true;
    }

    void applyRebuildsOnlyChangedSide()
    {
        QFETCH(bool, eth);
        const QString instrument = eth ? "ETH_USDC-PERPETUAL" : "BTC_USDC-PERPETUAL";
        const Int mid = (eth ? 2500 : 82500) * Scale, width = (eth ? 100 : 1000) * Scale;
        QTemporaryDir dir;
        const auto path = dir.filePath("selective.db");
        Engine seed;
        seed.profileId = "apply";
        seed.config.instrument = instrument.toStdString();
        seed.spec = {eth ? Scale / 100 : Scale / 10, Scale / 10000, {}};
        seed.config.longs = {mid - width, mid, 100 * Dollars, 3};
        seed.config.shorts = {mid, mid + width, 100 * Dollars, 3};
        seed.quote(mid, mid);
        seed.requestRebuild(seed.config);
        QHash<QString, QJsonObject> orders;
        qint64 timestamp = 1;
        const auto orderJson = [&](const Order& o) {
            return QJsonObject{{"instrument_name", instrument},
                {"order_id", QString::fromStdString(o.exchangeId)},
                {"label", QString::fromStdString(o.label)}, {"order_type", "limit"},
                {"order_state", "open"}, {"direction", seed.buys(o) ? "buy" : "sell"},
                {"price", double(o.price) / Scale},
                {"amount", double(o.total) * seed.spec.quantityStep / Scale},
                {"filled_amount", 0.0}, {"last_update_timestamp", ++timestamp}};
        };
        while (const auto action = seed.nextAction()) {
            const auto o = *seed.order(action->label);
            seed.apply({o.label, "ex-" + o.label, o.total, 0, ++timestamp, o.price, false});
            seed.resolved(o.label);
            const auto j = orderJson(*seed.order(o.label));
            orders.insert(j["order_id"].toString(), j);
        }
        QCOMPARE(orders.size(), 6);
        {
            Store db(path);
            db.save(seed);
        }
        const auto saved = [&] {
            Store db(path);
            Engine e;
            db.load(e);
            return e;
        };
        QStringList cancelled, created;
        QWebSocketServer server("Selective Apply", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto request = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = request["method"].toString();
                const auto params = request["params"].toObject();
                QJsonValue result = QJsonObject{};
                if (method == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "public/get_instrument")
                    result = QJsonObject{{"instrument_name", instrument},
                        {"base_currency", eth ? "ETH" : "BTC"}, {"settlement_currency", "USDC"},
                        {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"},
                        {"tick_size", eth ? 0.01 : 0.1}, {"min_trade_amount", 0.0001}};
                else if (method == "private/get_account_summary")
                    result = QJsonObject{{"id", eth ? 2945601 : 2945602},
                        {"limits", QJsonObject{{"matching_engine",
                                       QJsonObject{{"trading",
                                           QJsonObject{{"total", QJsonObject{{"rate", 5}}}}}}}}}};
                else if (method == "public/ticker")
                    result = QJsonObject{{"instrument_name", instrument},
                        {"best_bid_price", double(mid) / Scale - 0.01},
                        {"best_ask_price", double(mid) / Scale + 0.01}};
                else if (method == "private/get_position")
                    result = QJsonObject{{"size_currency", 0.0}};
                else if (method == "private/get_open_orders_by_instrument") {
                    QJsonArray open;
                    for (const auto& o : orders)
                        if (o["order_state"] == "open")
                            open.append(o);
                    result = open;
                } else if (method == "private/get_order_state") {
                    result = orders.value(params["order_id"].toString());
                } else if (method == "private/get_user_trades_by_order")
                    result = QJsonArray{};
                else if (method == "private/buy" || method == "private/sell") {
                    const auto id = "ex-" + params["label"].toString();
                    created.append(id);
                    const QJsonObject order{{"instrument_name", instrument}, {"order_id", id},
                        {"label", params["label"]}, {"order_type", "limit"},
                        {"order_state", "open"},
                        {"direction", method == "private/buy" ? "buy" : "sell"},
                        {"price", params["price"]}, {"amount", params["amount"]},
                        {"filled_amount", 0.0}, {"last_update_timestamp", ++timestamp}};
                    orders[id] = order;
                    result = QJsonObject{{"order", order}};
                } else if (method == "private/cancel") {
                    const auto id = params["order_id"].toString();
                    cancelled.append(id);
                    auto order = orders.value(id);
                    order["order_state"] = "cancelled";
                    order["last_update_timestamp"] = ++timestamp;
                    orders[id] = order;
                    result = order;
                }
                peer->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{"id", request["id"]}, {"result", result}})
                        .toJson()));
            });
        });
        Window window(
            path, instrument, nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_VERIFY2_WITH_TIMEOUT(
            window.findChild<QLabel*>("connectionStatus")->text() == "Connected",
            qPrintable(window.findChild<QPlainTextEdit*>()->toPlainText()), 5000);
        // Opening a profile intentionally pauses it; establish a running grid first.
        auto* start = window.findChild<QPushButton*>("start");
        QTRY_VERIFY(start->isEnabled());
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            return e.running && !e.rebuilding && e.occupiedSlots(Side::Long) == 3 &&
                   e.occupiedSlots(Side::Short) == 3 &&
                   std::all_of(e.orders().begin(), e.orders().end(), [](const Order& o) {
                       return o.mutation == Mutation::None;
                   });
        })(),
            7000);
        QHash<QString, QJsonObject> original;
        for (auto i = orders.cbegin(); i != orders.cend(); ++i)
            if (i.value()["order_state"] == "open")
                original.insert(i.key(), i.value());
        cancelled.clear();
        created.clear();
        QTableWidget* settings = nullptr;
        for (auto* table : window.findChildren<QTableWidget*>())
            if (table->rowCount() == 2 && table->columnCount() == 5)
                settings = table;
        QVERIFY(settings);
        auto* apply = window.findChild<QPushButton*>("applySettings");
        QTRY_VERIFY(apply->isEnabled());
        qobject_cast<QSpinBox*>(settings->cellWidget(0, 4))->setValue(4);
        apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            return !e.rebuilding && e.config.longs.levels == 4 &&
                   e.occupiedSlots(Side::Long) == 4 &&
                   std::all_of(e.orders().begin(), e.orders().end(), [](const Order& o) {
                       return o.mutation == Mutation::None;
                   });
        })(),
            7000);
        QCOMPARE(cancelled.size(), 3);
        QCOMPARE(created.size(), 4);
        for (const auto& id : cancelled)
            QCOMPARE(orders[id]["direction"].toString(), QString("buy"));
        for (const auto& o : original)
            if (o["direction"] == "sell")
                QCOMPARE(orders[o["order_id"].toString()], o);
        const auto after = saved();
        const auto shorts = after.rebuildFloor[1];
        QTRY_VERIFY(apply->isEnabled());
        apply->click();
        QTest::qWait(600);
        QCOMPARE(cancelled.size(), 3);
        QCOMPARE(created.size(), 4);
        QCOMPARE(saved().nextOrder, after.nextOrder);
        QVERIFY(window.findChild<QPlainTextEdit*>()->toPlainText().contains("Settings unchanged"));
        qobject_cast<QSpinBox*>(settings->cellWidget(1, 4))->setValue(2);
        QTRY_VERIFY(apply->isEnabled());
        apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            return !e.rebuilding && e.config.shorts.levels == 2 &&
                   e.occupiedSlots(Side::Short) == 2 &&
                   std::all_of(e.orders().begin(), e.orders().end(), [](const Order& o) {
                       return o.mutation == Mutation::None;
                   });
        })(),
            7000);
        QCOMPARE(cancelled.size(), 6);
        QCOMPARE(created.size(), 6);
        const auto final = saved();
        QCOMPARE(final.rebuildFloor[0], after.rebuildFloor[0]);
        QVERIFY(final.rebuildFloor[1] > shorts);
        for (const auto& o : after.orders())
            if (after.cycle(o.cycle)->side == Side::Long && o.live()) {
                QCOMPARE(final.order(o.label)->exchangeId, o.exchangeId);
                QCOMPARE(final.order(o.label)->status, Status::Open);
            }
    }

    void latencyIndicatorShowsSlowConnectionAndRecovery()
    {
        QTemporaryDir dir;
        QWebSocketServer server("UI latency", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QWebSocket* peer = nullptr;
        QJsonValue authId;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                if (j["method"] == "public/auth")
                    authId = j["id"];
                else if (j["method"] == "public/set_heartbeat")
                    peer->sendTextMessage(QString::fromUtf8(
                        QJsonDocument(QJsonObject{{"id", j["id"]}, {"result", QJsonObject{}}})
                            .toJson()));
                // Instrument initialization stays pending; this test never starts a grid.
            });
        });
        Window window(dir.filePath("latency.db"), "BTC_USDC-PERPETUAL", nullptr,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        auto* latency = window.findChild<QLabel*>("connectionLatency");
        QVERIFY(latency);
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_VERIFY(!authId.isUndefined());
        QTRY_VERIFY_WITH_TIMEOUT(
            latency->text().contains("≥") && latency->styleSheet().contains("#b91c1c"), 3500);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(
            QJsonObject{{"id", authId},
                {"result", QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}}}})
                .toJson()));
        QTRY_VERIFY_WITH_TIMEOUT(latency->text().contains(" ms") &&
                                     !latency->text().contains("≥") &&
                                     latency->styleSheet().isEmpty(),
            1000);
        peer->close();
        QTRY_COMPARE(latency->text(), QString("Latency: offline"));
        QVERIFY(latency->styleSheet().contains("#b91c1c"));
    }

    void transitionIndicatorUsesExistingStatusRow()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("transition.db");
        Engine seed;
        seed.profileId = "transitionui";
        seed.spec = {Scale / 10, Scale / 10000, {}};
        seed.config.longs = {82500 * Scale, 84500 * Scale, 850 * Dollars, 14};
        for (int i = 0; i < 6; ++i)
            seed.importCycle({seed.nextCycle++, 0, Side::Long, (83700 + i) * Scale,
                (83800 + i) * Scale, 2, 2, true});
        seed.rebuildFloor = {seed.nextCycle, seed.nextCycle};
        {
            Store db(path);
            db.save(seed);
        }
        Window window(path, "BTC_USDC-PERPETUAL");
        window.show();
        auto* latency = window.findChild<QLabel*>("connectionLatency");
        QVERIFY(latency);
        QCOMPARE(latency->text(), QString("Latency: — ms"));
        QVERIFY(latency->styleSheet().isEmpty());
        QVERIFY(latency->x() < window.findChild<QPushButton*>("deleteProfile")->x());
        QCOMPARE(latency->y(), window.findChild<QPushButton*>("deleteProfile")->y());
        auto* summary = window.findChild<QLabel*>("gridSummary");
        auto* button = window.findChild<QPushButton*>("applySettings");
        QVERIFY(summary && button);
        QVERIFY(summary->text().contains("transition: Long 8/14"));
        QVERIFY(button->property("transitionActive").toBool());
        QCOMPARE(button->palette().color(QPalette::Button), QColor("#dbeafe"));
        QCOMPARE(button->text(), QString("Apply settings / Rebuild"));
        QTest::qWait(150);
        QVERIFY(window.grab().save("rebuild-transition.png"));
        Window ordinary(dir.filePath("ordinary.db"), "BTC_USDC-PERPETUAL");
        QVERIFY(!ordinary.findChild<QPushButton*>("applySettings")
                ->property("transitionActive")
                .toBool());
        QVERIFY(!ordinary.findChild<QLabel*>("gridSummary")->text().contains("transition:"));
    }

    void missingCreateRecovery_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto& name : {"absent", "delayed-open", "historical-filled",
                 "historical-cancelled", "ambiguous", "history-error", "position-mismatch"})
            QTest::newRow(name) << QString(name);
    }

    void missingCreateRecovery()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("uncertain.db");
        Engine seed;
        seed.profileId = "missingcreate";
        seed.config.instrument = "ETH_USDC-PERPETUAL";
        seed.spec = {Scale / 100, Scale / 10000, {}};
        seed.externalPosition = 0;
        seed.config.longs = {2480 * Scale, 2500 * Scale, 100 * Dollars, 2};
        seed.importCycle({1, 1, Side::Long, 2490 * Scale, 2500 * Scale, 40, 0, false});
        Order pending;
        pending.label = "dg_missingcreate_1_e_1";
        pending.cycle = 1;
        pending.price = 2490 * Scale;
        pending.total = pending.desiredTotal = 40;
        pending.status = Status::Uncertain;
        pending.mutation = Mutation::Create;
        seed.importOrder(pending);
        seed.nextCycle = seed.nextOrder = 2;
        seed.blocked = true;
        {
            Store db(path);
            db.save(seed);
        }
        const auto saved = [&] {
            Store db(path);
            Engine e;
            db.load(e);
            return e;
        };
        QJsonObject accepted{{"instrument_name", "ETH_USDC-PERPETUAL"},
            {"order_id", "exchange-entry"}, {"label", QString::fromStdString(pending.label)},
            {"order_type", "limit"}, {"order_state", "open"}, {"direction", "buy"},
            {"price", 2490.0}, {"amount", 0.004}, {"filled_amount", 0.0},
            {"last_update_timestamp", 1}};
        if (scenario.startsWith("historical"))
            accepted["order_state"] = scenario == "historical-filled" ? "filled" : "cancelled";
        if (scenario == "historical-filled")
            accepted["filled_amount"] = 0.004;
        int openReads = 0, historyReads = 0, entryCreates = 0, takeCreates = 0;
        bool paginationSeen = false, historyFlagsOK = true;
        QHash<QString, QJsonObject> placed;
        QWebSocketServer server("Missing create recovery", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = j["method"].toString();
                const auto p = j["params"].toObject();
                QJsonValue result = QJsonObject{{"ok", true}};
                QJsonObject error;
                if (method == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary")
                    result = QJsonObject{{"id", 9854321}};
                else if (method == "public/get_instrument")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"base_currency", "ETH"}, {"settlement_currency", "USDC"},
                        {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"},
                        {"tick_size", 0.01}, {"min_trade_amount", 0.0001},
                        {"contract_size", 0.0001}};
                else if (method == "public/ticker")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"best_bid_price", 2499.99}, {"best_ask_price", 2500.01}};
                else if (method == "private/get_order_state_by_label")
                    result =
                        scenario == "ambiguous" ? QJsonArray{accepted, accepted} : QJsonArray{};
                else if (method == "private/get_open_orders_by_instrument") {
                    ++openReads;
                    QJsonArray open;
                    if (scenario == "delayed-open" && openReads > 1 &&
                        accepted["order_state"] == "open")
                        open.append(accepted);
                    for (const auto& o : placed)
                        if (o["order_state"] == "open")
                            open.append(o);
                    result = open;
                } else if (method == "private/get_order_history_by_instrument") {
                    ++historyReads;
                    historyFlagsOK &= p["include_old"].toBool() && p["include_unfilled"].toBool() &&
                                      p["count"].toInt() == 1000;
                    if (scenario == "history-error")
                        error = {{"code", 10050}, {"message", "simulated history failure"}};
                    QJsonArray history;
                    if (scenario == "historical-filled" && !p["historical"].toBool()) {
                        if (p["offset"].toInt() == 0)
                            for (int i = 0; i < 1000; ++i)
                                history.append(QJsonObject{
                                    {"order_id", QString::number(i)}, {"label", "unrelated"}});
                        else
                            paginationSeen = p["offset"].toInt() == 1000;
                    }
                    if (scenario.startsWith("historical") && p["historical"].toBool())
                        history.append(accepted);
                    result = history;
                } else if (method == "private/get_position")
                    result =
                        QJsonObject{{"size_currency", scenario == "historical-filled"   ? 0.004
                                                      : scenario == "position-mismatch" ? 0.0001
                                                                                        : 0.0}};
                else if (method == "private/get_user_trades_by_order")
                    result = QJsonArray{};
                else if (method == "private/buy" || method == "private/sell") {
                    if (p["label"].toString().contains("_e_"))
                        ++entryCreates;
                    else
                        ++takeCreates;
                    auto o = accepted;
                    o["order_id"] = "take-" + p["label"].toString();
                    o["label"] = p["label"];
                    o["direction"] = method == "private/buy" ? "buy" : "sell";
                    o["price"] = p["price"];
                    o["amount"] = p["amount"];
                    o["filled_amount"] = 0.0;
                    o["order_state"] = "open";
                    o["last_update_timestamp"] = 2;
                    placed.insert(o["order_id"].toString(), o);
                    result = QJsonObject{{"order", o}};
                } else if (method == "private/cancel") {
                    accepted["order_state"] = "cancelled";
                    accepted["last_update_timestamp"] = 2;
                    result = accepted;
                }
                QJsonObject response{{"jsonrpc", "2.0"}, {"id", j["id"]}};
                response.insert(error.isEmpty() ? "result" : "error",
                    error.isEmpty() ? result : QJsonValue(error));
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(response).toJson()));
            });
        });
        Window window(path, "ETH_USDC-PERPETUAL", nullptr,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        auto* start = window.findChild<QPushButton*>("start");
        auto* journal = window.findChild<QPlainTextEdit*>();
        const bool failure = scenario == "ambiguous" || scenario == "history-error" ||
                             scenario == "position-mismatch";
        if (failure) {
            QTRY_VERIFY_WITH_TIMEOUT(
                journal->toPlainText().contains(scenario == "ambiguous" ? "invalid or ambiguous"
                                                : scenario == "history-error"
                                                    ? "history reconciliation failed"
                                                    : "position does not match"),
                10000);
            QVERIFY(!start->isEnabled());
            QCOMPARE(saved().order(pending.label)->mutation, Mutation::Create);
            QVERIFY(saved().order(pending.label)->exchangeId.empty());
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 10000);
            QTRY_COMPARE_WITH_TIMEOUT(saved().order(pending.label)->mutation, Mutation::None, 5000);
            if (scenario == "absent") {
                QCOMPARE(saved().order(pending.label)->status, Status::Rejected);
                QVERIFY(openReads >= 2);
                QVERIFY(historyReads >= 4);
            } else if (scenario == "historical-filled") {
                QTRY_COMPARE_WITH_TIMEOUT(takeCreates, 1, 5000);
                QCOMPARE(saved().trackedPosition(), Int(400000));
                QVERIFY(paginationSeen);
            } else
                QTRY_COMPARE_WITH_TIMEOUT(
                    saved().order(pending.label)->status, Status::Terminal, 5000);
        }
        QVERIFY(window.findChild<QPushButton*>("cancelBotOrders")->isEnabled());
        QVERIFY(window.findChild<QPushButton*>("deleteProfile")->isEnabled());
        QCOMPARE(entryCreates, 0);
        QVERIFY(historyFlagsOK);
        QCOMPARE(saved().profileId, seed.profileId);
    }

    void cancellationAndProfileResetWhileBlocked_data()
    {
        QTest::addColumn<bool>("erase");
        QTest::addColumn<bool>("cancelError");
        QTest::addColumn<bool>("verificationError");
        QTest::addColumn<bool>("positionError");
        QTest::newRow("cancel-only") << false << false << false << false;
        QTest::newRow("delete-current-only") << true << false << false << false;
        QTest::newRow("failed-cancel-retains-profile") << true << true << false << false;
        QTest::newRow("failed-verification-retains-profile") << true << false << true << false;
        QTest::newRow("failed-position-read-retains-profile") << true << false << false << true;
    }

    void cancellationAndProfileResetWhileBlocked()
    {
        QFETCH(bool, erase);
        QFETCH(bool, cancelError);
        QFETCH(bool, verificationError);
        QFETCH(bool, positionError);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("blocked.db"),
                   otherPath = directory.filePath("other.db");
        Engine seed;
        seed.profileId = "blockedprofile";
        seed.config.instrument = "ETH_USDC-PERPETUAL";
        seed.spec = {Scale / 100, Scale / 10000, {}};
        seed.externalPosition = 0;
        seed.config.longs = {2480 * Scale, 2500 * Scale, 100 * Dollars, 2};
        seed.importCycle({1, 1, Side::Long, 2490 * Scale, 2500 * Scale, 40, 0, false});
        Order intent;
        intent.label = "dg_blockedprofile_1_e_1";
        intent.cycle = 1;
        intent.price = 2490 * Scale;
        intent.total = intent.desiredTotal = 40;
        intent.mutation = Mutation::Create;
        intent.status = Status::Uncertain;
        seed.importOrder(intent);
        seed.nextCycle = seed.nextOrder = 2;
        {
            Store db(path);
            db.save(seed);
            Store other(otherPath);
            other.save(seed);
        }
        const auto saved = [&](const QString& file) {
            Store db(file);
            Engine e;
            db.load(e);
            return e;
        };
        QJsonObject own{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"order_id", "own-exchange"},
            {"label", QString::fromStdString(intent.label)}, {"order_type", "limit"},
            {"order_state", "open"}, {"direction", "buy"}, {"price", 2490.0}, {"amount", 0.004},
            {"filled_amount", 0.0}, {"last_update_timestamp", 1}};
        auto manual = own;
        manual["order_id"] = "manual-exchange";
        manual["label"] = "manual";
        auto otherBot = own;
        otherBot["order_id"] = "other-exchange";
        otherBot["label"] = "dg_otherprofile_1_e_1";
        QStringList cancelled;
        int entryCreates = 0;
        QWebSocketServer server("Blocked cancellation", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = j["method"].toString();
                const auto p = j["params"].toObject();
                QJsonValue result = QJsonObject{{"ok", true}};
                QJsonObject error;
                if (method == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary")
                    result = QJsonObject{{"id", 4854321}};
                else if (method == "public/get_instrument")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"base_currency", "ETH"}, {"settlement_currency", "USDC"},
                        {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"},
                        {"tick_size", 0.01}, {"min_trade_amount", 0.0001},
                        {"contract_size", 0.0001}};
                else if (method == "public/ticker")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"best_bid_price", 2499.99}, {"best_ask_price", 2500.01}};
                else if (method == "private/get_order_state_by_label")
                    result = own["order_state"] == "open" ? QJsonValue(QJsonObject{})
                                                          : QJsonValue(QJsonArray{own});
                else if (method == "private/get_open_orders_by_instrument") {
                    QJsonArray open{manual, otherBot};
                    if (own["order_state"] == "open")
                        open.append(own);
                    result = open;
                    if (verificationError && !cancelled.isEmpty())
                        error = {{"code", 10050}, {"message", "simulated verification failure"}};
                } else if (method == "private/cancel_by_label") {
                    cancelled.append(p["label"].toString());
                    if (cancelError)
                        error = {{"code", 10050}, {"message", "simulated cancellation failure"}};
                    else {
                        own["order_state"] = "cancelled";
                        own["last_update_timestamp"] = 2;
                        result = 1;
                    }
                } else if (method == "private/get_position") {
                    result = QJsonObject{{"size_currency", erase ? 0.004 : 0.0}};
                    if (positionError && !cancelled.isEmpty())
                        error = {{"code", 10050}, {"message", "simulated position read failure"}};
                } else if (method == "private/get_user_trades_by_order")
                    result = QJsonArray{};
                else if (method == "private/buy" || method == "private/sell")
                    ++entryCreates;
                QJsonObject response{{"jsonrpc", "2.0"}, {"id", j["id"]}};
                response.insert(error.isEmpty() ? "result" : "error",
                    error.isEmpty() ? result : QJsonValue(error));
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(response).toJson()));
            });
        });
        Window window(path, "ETH_USDC-PERPETUAL", nullptr,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        auto* journal = window.findChild<QPlainTextEdit*>();
        QTRY_VERIFY_WITH_TIMEOUT(journal->toPlainText().contains("invalid or ambiguous"), 5000);
        QVERIFY(!window.findChild<QPushButton*>("start")->isEnabled());
        auto* cancel = window.findChild<QPushButton*>(erase ? "deleteProfile" : "cancelBotOrders");
        QVERIFY(cancel->isEnabled());
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                dialog->button(QMessageBox::Yes)->click();
        });
        cancel->click();
        if (cancelError || verificationError || positionError) {
            QTRY_VERIFY_WITH_TIMEOUT(journal->toPlainText().contains("profile retained"), 5000);
            QCOMPARE(saved(path).profileId, seed.profileId);
            QCOMPARE(saved(path).orders().size(), size_t(1));
            QVERIFY(cancel->isEnabled());
        } else if (erase) {
            QTRY_VERIFY_WITH_TIMEOUT(saved(path).profileId != seed.profileId, 8000);
            QVERIFY(saved(path).orders().empty());
            QVERIFY(saved(path).cycles().empty());
            QCOMPARE(saved(path).config.longs.budget, Int(0));
            QCOMPARE(saved(path).externalPosition, std::optional<Int>(400000));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("start")->isEnabled(), 8000);
            QCOMPARE(saved(path).profileId, seed.profileId);
            QCOMPARE(saved(path).order(intent.label)->status, Status::Terminal);
        }
        QCOMPARE(cancelled, QStringList{QString::fromStdString(intent.label)});
        QCOMPARE(entryCreates, 0);
        QCOMPARE(saved(otherPath).profileId, seed.profileId);
        QCOMPARE(saved(otherPath).orders().size(), size_t(1));
        QCOMPARE(manual["order_state"].toString(), QString("open"));
        QCOMPARE(otherBot["order_state"].toString(), QString("open"));
    }

    void fillsDoNotLeaveFiveSecondTakeGaps_data()
    {
        QTest::addColumn<bool>("eth");
        QTest::newRow("BTC") << false;
        QTest::newRow("ETH") << true;
    }

    void fillsDoNotLeaveFiveSecondTakeGaps()
    {
        QFETCH(bool, eth);
        const QString instrument = eth ? "ETH_USDC-PERPETUAL" : "BTC_USDC-PERPETUAL";
        const Int mid = (eth ? 2500 : 82500) * Scale, width = (eth ? 20 : 200) * Scale;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("latency.db");
        Engine seed;
        seed.profileId = eth ? "latencyeth" : "latencybtc";
        seed.config.instrument = instrument.toStdString();
        seed.config.longs = {mid - width, mid, 840 * Dollars, 14};
        seed.config.shorts = {mid, mid + width, 500 * Dollars, 14};
        {
            Store db(path);
            db.save(seed);
        }
        QHash<QString, QJsonObject> orders;
        QStringList stateReads, cancelled;
        QList<qint64> takeLatencies;
        int openReads = 0, tickerReads = 0;
        qint64 timestamp = 1;
        double externalChange = 0;
        bool overlappingMutations = false;
        QElapsedTimer protectionClock;
        QWebSocket* peer = nullptr;
        QJsonObject delayedFill;
        bool deliverDelayedFill = false, recovering = false;
        const auto publish = [&](const QJsonArray& events,
                                 const QJsonArray& trades = QJsonArray{}) {
            const QJsonObject params{{"channel", "user.changes." + instrument + ".100ms"},
                {"data", QJsonObject{{"orders", events}, {"trades", trades}}}};
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonObject{{"method", "subscription"}, {"params", params}})
                    .toJson()));
        };
        const auto trade = [&](const QJsonObject& o, double quantity) {
            return QJsonObject{{"instrument_name", instrument}, {"order_id", o["order_id"]},
                {"label", o["label"]}, {"trade_id", "trade-" + QString::number(++timestamp)},
                {"timestamp", QDateTime::currentMSecsSinceEpoch()}, {"amount", quantity},
                {"fee", 0.0}, {"fee_currency", "USDC"}};
        };
        const auto saved = [&] {
            Store db(path);
            Engine e;
            db.load(e);
            return e;
        };
        const auto settleAccountRead = [&] {
            QTest::qWait(50);
            const int before = tickerReads;
            publish({}, QJsonArray{QJsonObject{
                            {"instrument_name", instrument}, {"order_id", "read-barrier"}}});
            if (!QTest::qWaitFor(
                    [&] {
                        return tickerReads > before;
                    },
                    6000))
                return false;
            QTest::qWait(50);
            return true;
        };
        const auto takeFor = [&](const QString& entryId) {
            const auto label = orders.value(entryId)["label"].toString();
            const auto prefix = label.left(label.indexOf("_e_")) + "_t_";
            for (auto i = orders.cbegin(); i != orders.cend(); ++i)
                if (i.value()["label"].toString().startsWith(prefix) &&
                    i.value()["order_state"] == "open")
                    return i.key();
            return QString{};
        };
        QWebSocketServer server("Fill latency", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = j["method"].toString();
                const auto p = j["params"].toObject();
                if (method == "private/buy" || method == "private/sell" ||
                    method == "private/edit" || method == "private/cancel") {
                    const auto e = saved();
                    if (std::count_if(e.orders().begin(), e.orders().end(), [](const Order& o) {
                            return o.mutation != Mutation::None;
                        }) != 1)
                        overlappingMutations = true;
                }
                if (recovering)
                    qInfo() << instrument << "recovery ms:" << protectionClock.elapsed() << method;
                QJsonValue result = QJsonObject{{"ok", true}};
                if (method == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary")
                    result = QJsonObject{{"id", 123454321},
                        {"limits", QJsonObject{{"matching_engine",
                                       QJsonObject{{"trading",
                                           QJsonObject{{"total", QJsonObject{{"rate", 100}}}}}}}}}};
                else if (method == "public/get_instrument")
                    result = QJsonObject{{"instrument_name", instrument},
                        {"base_currency", eth ? "ETH" : "BTC"}, {"settlement_currency", "USDC"},
                        {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"},
                        {"tick_size", eth ? 0.01 : 0.1}, {"min_trade_amount", 0.0001},
                        {"contract_size", 0.0001}};
                else if (method == "public/ticker") {
                    ++tickerReads;
                    result = QJsonObject{{"instrument_name", instrument},
                        {"best_bid_price", double(mid) / Scale - 0.01},
                        {"best_ask_price", double(mid) / Scale + 0.01}};
                } else if (method == "private/get_position") {
                    double actual = externalChange;
                    for (const auto& o : orders)
                        actual +=
                            (o["direction"] == "buy" ? 1 : -1) * o["filled_amount"].toDouble();
                    result = QJsonObject{{"size_currency", actual}};
                    if (deliverDelayedFill) {
                        deliverDelayedFill = false;
                        QTimer::singleShot(100, &server, [&] {
                            publish(QJsonArray{delayedFill, delayedFill});
                        });
                    }
                } else if (method == "private/get_open_orders_by_instrument") {
                    ++openReads;
                    QJsonArray open;
                    for (const auto& o : orders)
                        if (o["order_state"] == "open")
                            open.append(o);
                    result = open;
                } else if (method == "private/get_order_state") {
                    const auto id = p["order_id"].toString();
                    stateReads.append(id);
                    result = orders.value(id);
                } else if (method == "private/get_user_trades_by_order")
                    result = QJsonArray{};
                else if (method == "private/buy" || method == "private/sell") {
                    const auto id = "sim-" + p["label"].toString();
                    const QJsonObject o{{"instrument_name", instrument}, {"order_id", id},
                        {"label", p["label"]}, {"order_type", "limit"}, {"order_state", "open"},
                        {"direction", method == "private/buy" ? "buy" : "sell"},
                        {"price", p["price"]}, {"amount", p["amount"]}, {"filled_amount", 0.0},
                        {"last_update_timestamp", ++timestamp}};
                    orders.insert(id, o);
                    result = QJsonObject{{"order", o}};
                    if (p["label"].toString().contains("_t_") && protectionClock.isValid())
                        takeLatencies.append(protectionClock.elapsed());
                } else if (method == "private/edit" || method == "private/cancel") {
                    const auto id = p["order_id"].toString();
                    auto o = orders.value(id);
                    if (method == "private/edit")
                        o["amount"] = p["amount"];
                    else {
                        cancelled.append(id);
                        o["order_state"] = "cancelled";
                    }
                    o["last_update_timestamp"] = ++timestamp;
                    orders[id] = o;
                    result = o;
                }
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(
                    QJsonObject{{"jsonrpc", "2.0"}, {"id", j["id"]}, {"result", result}})
                        .toJson()));
            });
        });
        Window window(
            path, instrument, nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_COMPARE_WITH_TIMEOUT(
            window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
        QTRY_VERIFY(window.findChild<QPushButton*>("start")->isEnabled());
        QElapsedTimer placementClock;
        placementClock.start();
        window.findChild<QPushButton*>("start")->click();
        QTRY_COMPARE_WITH_TIMEOUT(orders.size(), 28, 18000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            return std::all_of(e.orders().begin(), e.orders().end(), [](const Order& o) {
                return o.mutation == Mutation::None;
            });
        })(),
            1000);
        const auto placementMs = placementClock.elapsed();
        QVERIFY2(placementMs < 16000, "Entry placement adds a pump-timer gap to 500 ms API pacing");

        // A full rebuild must pace both cancellation and replacement by replies, with one pending mutation.
        QTRY_VERIFY(window.findChild<QPushButton*>("start")->isEnabled());
        placementClock.restart();
        window.findChild<QPushButton*>("start")->click();
        QTRY_COMPARE_WITH_TIMEOUT(orders.size(), 56, 35000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            return std::all_of(e.orders().begin(), e.orders().end(), [](const Order& o) {
                return o.mutation == Mutation::None;
            });
        })(),
            1000);
        const auto rebuildMs = placementClock.elapsed();
        QCOMPARE(cancelled.size(), 28);
        QVERIFY2(
            rebuildMs < 32000, "Rebuild adds a pump-timer gap to 500 ms API pacing");
        QVERIFY(!overlappingMutations);
        cancelled.clear();
        qInfo() << instrument << "28 entries ms:" << placementMs
                << "28 cancels + 28 replacement entries ms:" << rebuildMs;
        QStringList shorts;
        for (auto i = orders.cbegin(); i != orders.cend(); ++i)
            if (i.value()["direction"] == "sell" && i.value()["order_state"] == "open")
                shorts.append(i.key());
        std::sort(shorts.begin(), shorts.end(), [&](const QString& a, const QString& b) {
            return orders[a]["price"].toDouble() < orders[b]["price"].toDouble();
        });
        QCOMPARE(shorts.size(), 14);
        QCOMPARE(saved().availableSlots(Side::Short), 14);
        QVERIFY(settleAccountRead());

        // The REST position overtakes a batched order update; duplicate updates must not double inventory.
        const int readsBeforeDelay = openReads;
        auto& first = orders[shorts[0]];
        first["filled_amount"] = 0.0002;
        first["last_update_timestamp"] = ++timestamp;
        delayedFill = first;
        deliverDelayedFill = true;
        protectionClock.start();
        publish({}, QJsonArray{trade(first, 0.0002)});
        QTRY_VERIFY_WITH_TIMEOUT(!takeFor(shorts[0]).isEmpty(), 1000);
        QVERIFY(!takeLatencies.isEmpty());
        QVERIFY(takeLatencies.back() <= 1000);
        const auto ordinaryLatency = takeLatencies.back();
        QTRY_COMPARE_WITH_TIMEOUT(saved().availableSlots(Side::Short), 13, 1000);
        QCOMPARE(saved().trackedPosition(), Int(-20000));
        QCOMPARE(openReads, readsBeforeDelay);
        QVERIFY(cancelled.isEmpty());
        QVERIFY(stateReads.isEmpty());

        // Partial and final take fills release inventory once, even with duplicates and a stale snapshot.
        const auto firstTakeId = takeFor(shorts[0]);
        auto& firstTake = orders[firstTakeId];
        firstTake["filled_amount"] = 0.0001;
        firstTake["last_update_timestamp"] = ++timestamp;
        const auto partialTake = firstTake;
        publish(QJsonArray{firstTake, firstTake}, QJsonArray{trade(firstTake, 0.0001)});
        QTRY_COMPARE_WITH_TIMEOUT(saved().trackedPosition(), Int(-10000), 1000);
        QCOMPARE(saved().availableSlots(Side::Short), 13);
        firstTake["filled_amount"] = 0.0002;
        firstTake["order_state"] = "filled";
        firstTake["last_update_timestamp"] = ++timestamp;
        publish(
            QJsonArray{firstTake, firstTake, partialTake}, QJsonArray{trade(firstTake, 0.0001)});
        QTRY_COMPARE_WITH_TIMEOUT(saved().availableSlots(Side::Short), 14, 1000);
        // Wait for the repeat entry to be fully replenished, not just for a gap between two edits.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            const auto* o = e.order(orders.value(shorts[0])["label"].toString().toStdString());
            return o && o->mutation == Mutation::None &&
                   o->remaining() == e.cycle(o->cycle)->capacity;
        })(),
            1500);
        QVERIFY(settleAccountRead());

        // Lose two terminal entry notifications. Only those absent from the bulk snapshot need individual reads.
        const int readsBeforeLoss = openReads;
        stateReads.clear();
        takeLatencies.clear();
        QJsonArray lostTrades;
        const QStringList lostEntries{shorts[0], shorts[1]};
        Int lostLots = 0;
        for (const auto& id : lostEntries) {
            auto& o = orders[id];
            const double delta = o["amount"].toDouble() - o["filled_amount"].toDouble();
            o["filled_amount"] = o["amount"];
            o["order_state"] = "filled";
            o["last_update_timestamp"] = ++timestamp;
            lostLots += Int(std::llround(delta * 10000));
            lostTrades.append(trade(o, delta));
        }
        protectionClock.restart();
        recovering = true;
        publish({}, lostTrades);
        QTRY_VERIFY_WITH_TIMEOUT(
            !takeFor(lostEntries[0]).isEmpty() && !takeFor(lostEntries[1]).isEmpty(), 1500);
        QCOMPARE(takeLatencies.size(), 2);
        QVERIFY(takeLatencies.back() <= 1500);
        recovering = false;
        QCOMPARE(openReads, readsBeforeLoss + 1);
        QCOMPARE(stateReads.size(), 2);
        QVERIFY(stateReads.contains(lostEntries[0]));
        QVERIFY(stateReads.contains(lostEntries[1]));
        QVERIFY(cancelled.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(saved().trackedPosition(), -lostLots * (Scale / 10000), 1000);
        QCOMPARE(saved().availableSlots(Side::Short), 12);
        QVERIFY(saved().running);
        QVERIFY(!saved().positionBlocked);
        QVERIFY(window.findChild<QPlainTextEdit*>()->toPlainText().contains(
            "account position. Running."));

        // Reuse the existing exchange simulator to exercise a real reconnect in both instruments.
        QVERIFY(settleAccountRead());
        const auto ordersBeforeReconnect = orders.size();
        const auto cancellationsBeforeReconnect = cancelled.size();
        peer->close();
        QTRY_COMPARE_WITH_TIMEOUT(
            window.findChild<QLabel*>("connectionStatus")->text(), QString("Disconnected"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 10000);
        QVERIFY(saved().running);
        QVERIFY(!saved().rebuilding);
        QCOMPARE(orders.size(), ordersBeforeReconnect);
        QCOMPARE(cancelled.size(), cancellationsBeforeReconnect);

        // A real external change still blocks entries and keeps the accepted takes intact.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto e = saved();
            return std::all_of(e.orders().begin(), e.orders().end(), [](const Order& o) {
                return o.mutation == Mutation::None;
            });
        })(),
            1000);
        QVERIFY(settleAccountRead());
        externalChange = 0.0001;
        publish({}, QJsonArray{QJsonObject{{"instrument_name", instrument}, {"order_id", "manual"},
                        {"amount", 0.0001}}});
        QTRY_VERIFY_WITH_TIMEOUT(saved().positionBlocked, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!cancelled.isEmpty(), 1500);
        for (const auto& id : cancelled)
            QVERIFY(!orders[id]["label"].toString().contains("_t_"));
        QCOMPARE(*saved().externalPosition, Int(0));
        QVERIFY(!saved().running);
        const auto journal = window.findChild<QPlainTextEdit*>()->toPlainText();
        QVERIFY(journal.contains("external=0.00000000 coin"));
        QVERIFY(journal.contains("difference=0.00010000 coin"));
        qInfo() << instrument << "delayed-fill take ms:" << ordinaryLatency
                << "recovered takes ms:" << takeLatencies
                << "individual recovery reads:" << stateReads.size();
    }

    void manualAndExistingBotTakesSurviveRestart_data()
    {
        QTest::addColumn<bool>("trigger");
        QTest::newRow("manual-limit") << false;
        QTest::newRow("manual-trigger-market") << true;
    }

    void manualAndExistingBotTakesSurviveRestart()
    {
        QFETCH(bool, trigger);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("takes.db");
        Engine seed;
        seed.profileId = "preservetakes";
        seed.config.instrument = "ETH_USDC-PERPETUAL";
        seed.spec = {Scale / 100, Scale / 10000, {}};
        seed.externalPosition = 4000000;
        seed.config.longs = {2480 * Scale, 2500 * Scale, 100 * Dollars, 2};
        seed.config.shorts = {2500 * Scale, 2520 * Scale, 100 * Dollars, 2};
        seed.importCycle(
            {seed.nextCycle++, 7, Side::Short, 2506 * Scale, 2504 * Scale, 40, 40, true});
        Order saved;
        saved.label = "dg_preservetakes_1_t_1";
        saved.exchangeId = "existing-bot-take";
        saved.cycle = 1;
        saved.role = Role::Take;
        saved.price = 2504 * Scale;
        saved.total = 40;
        saved.status = Status::Open;
        seed.importOrder(saved);
        seed.nextOrder = 2;
        {
            Store db(path);
            db.save(seed);
        }
        const QJsonObject manual{{"instrument_name", "ETH_USDC-PERPETUAL"},
            {"order_id", "USDC-210049237502281438"}, {"label", "manual"},
            {"order_type", trigger ? "take_market" : "limit"},
            {"order_state", trigger ? "untriggered" : "open"}, {"direction", "sell"},
            {"price", trigger ? QJsonValue("market_price") : QJsonValue(2510.0)},
            {"reduce_only", true}, {"amount", 0.04}, {"filled_amount", 0.0}};
        const QJsonObject take{{"instrument_name", "ETH_USDC-PERPETUAL"},
            {"order_id", "existing-bot-take"}, {"label", QString::fromStdString(saved.label)},
            {"order_type", "limit"}, {"order_state", "open"}, {"direction", "buy"},
            {"price", 2504.0}, {"amount", 0.004}, {"filled_amount", 0.0},
            {"last_update_timestamp", 1}};
        QHash<QString, QJsonObject> entries;
        QStringList cancelled, edited;
        int takeCreates = 0;
        QWebSocketServer server("Preserve takes", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = j["method"].toString();
                const auto p = j["params"].toObject();
                QJsonValue result = QJsonObject{{"ok", true}};
                if (method == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary")
                    result = QJsonObject{{"id", 7654321}};
                else if (method == "public/get_instrument")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"base_currency", "ETH"}, {"settlement_currency", "USDC"},
                        {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"},
                        {"tick_size", 0.01}, {"min_trade_amount", 0.0001},
                        {"contract_size", 0.0001}};
                else if (method == "private/get_position")
                    result = QJsonObject{{"size_currency", 0.036}};
                else if (method == "public/ticker")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"best_bid_price", 2505.07}, {"best_ask_price", 2505.08}};
                else if (method == "private/get_open_orders_by_instrument") {
                    QJsonArray open{manual, take};
                    for (const auto& o : entries)
                        if (o["order_state"] == "open")
                            open.append(o);
                    result = open;
                } else if (method == "private/get_order_state")
                    result = p["order_id"] == "existing-bot-take"
                                 ? take
                                 : entries.value(p["order_id"].toString());
                else if (method == "private/buy" || method == "private/sell") {
                    if (p["label"].toString().contains("_t_"))
                        ++takeCreates;
                    const QString id = "entry-" + p["label"].toString();
                    const QJsonObject o{{"instrument_name", "ETH_USDC-PERPETUAL"}, {"order_id", id},
                        {"label", p["label"]}, {"order_type", "limit"}, {"order_state", "open"},
                        {"direction", method == "private/buy" ? "buy" : "sell"},
                        {"price", p["price"]}, {"amount", p["amount"]}, {"filled_amount", 0.0},
                        {"last_update_timestamp", 1}};
                    entries[id] = o;
                    result = QJsonObject{{"order", o}};
                } else if (method == "private/cancel") {
                    const QString id = p["order_id"].toString();
                    cancelled.append(id);
                    auto o = entries.value(id);
                    o["order_state"] = "cancelled";
                    o["last_update_timestamp"] = 2;
                    entries[id] = o;
                    result = o;
                } else if (method == "private/edit") {
                    edited.append(p["order_id"].toString());
                    result = QJsonObject{{"order", take}};
                }
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(
                    QJsonObject{{"jsonrpc", "2.0"}, {"id", j["id"]}, {"result", result}})
                        .toJson()));
            });
        });
        for (int run = 0; run < 2; ++run) {
            auto window = std::make_unique<Window>(path, "ETH_USDC-PERPETUAL", nullptr,
                QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
            window->findChild<QLineEdit*>("clientId")->setText("fake");
            window->findChild<QLineEdit*>("clientSecret")->setText("secret");
            window->findChild<QPushButton*>("connect")->click();
            QTRY_COMPARE_WITH_TIMEOUT(
                window->findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
            QTRY_VERIFY(window->findChild<QPushButton*>("start")->isEnabled());
            window->findChild<QPushButton*>("start")->click();
            if (!trigger)
                QTRY_VERIFY(entries.size() >= 2 * (run + 1));
            else
                QTest::qWait(850);
            if (trigger)
                QVERIFY(entries.isEmpty());
            for (const auto& o : entries)
                QCOMPARE(o["direction"].toString(), QString("buy"));
            window->findChild<QPushButton*>("pause")->click();
            QTRY_VERIFY_WITH_TIMEOUT(([&] {
                Store db(path);
                Engine e;
                db.load(e);
                return e.settled() && !e.running;
            })(),
                5000);
            Engine restored;
            {
                Store db(path);
                QVERIFY(db.load(restored));
            }
            const auto* restoredTake = restored.order(saved.label);
            QVERIFY(restoredTake);
            QVERIFY(restoredTake->live());
            QCOMPARE(restoredTake->exchangeId, saved.exchangeId);
            QCOMPARE(restoredTake->price, saved.price);
            QCOMPARE(restoredTake->total, saved.total);
            QCOMPARE(restored.foreign.size(),
                size_t(
                    0)); // Foreign orders are read from the exchange, never adopted into saved cycles.
            QCOMPARE(takeCreates, 0);
            QVERIFY(edited.isEmpty());
            QVERIFY(!cancelled.contains("existing-bot-take"));
            QVERIFY(!cancelled.contains("USDC-210049237502281438"));
            window->close();
            QTest::qWait(500);
            window.reset();
        }
    }

    void restartAfterManualFlattenDoesNotRestoreOldTakes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("old.db");
        Engine seed;
        seed.profileId = "oldeth";
        seed.config.instrument = "ETH_USDC-PERPETUAL";
        seed.spec = {Scale / 100, Scale / 10000, {}};
        seed.config.longs = {2480 * Scale, 2500 * Scale, 1000 * Dollars, 10};
        seed.config.shorts = {2500 * Scale, 2520 * Scale, 1000 * Dollars, 10};
        for (int i = 0; i < 3; ++i)
            seed.importCycle({seed.nextCycle++, 7, Side::Long, 2500 * Scale - (i + 1) * Scale / 5,
                2500 * Scale - i * Scale / 5, 40, 40, true});
        {
            Store db(path);
            db.save(seed);
        }
        QWebSocketServer server("Manual flatten", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int tradingCalls = 0;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, this, [&, peer](const QString& frame) {
                const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                const auto method = j["method"].toString();
                QJsonValue result = QJsonObject{{"ok", true}};
                if (method == "public/auth")
                    result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                else if (method == "private/get_account_summary")
                    result = QJsonObject{{"id", 87654321}};
                else if (method == "public/get_instrument")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"base_currency", "ETH"}, {"settlement_currency", "USDC"},
                        {"kind", "future"}, {"settlement_period", "perpetual"}, {"state", "open"},
                        {"tick_size", 0.01}, {"min_trade_amount", 0.0001},
                        {"contract_size", 0.0001}};
                else if (method == "private/get_open_orders_by_instrument")
                    result = QJsonArray{};
                else if (method == "private/get_position")
                    result = QJsonObject{{"size_currency", 0.0}};
                else if (method == "public/ticker")
                    result = QJsonObject{{"instrument_name", "ETH_USDC-PERPETUAL"},
                        {"best_bid_price", 2500.19}, {"best_ask_price", 2500.39}};
                else if (method == "private/buy" || method == "private/sell" ||
                         method == "private/edit")
                    ++tradingCalls;
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(
                    QJsonObject{{"jsonrpc", "2.0"}, {"id", j["id"]}, {"result", result}})
                        .toJson()));
            });
        });
        Window window(path, "ETH_USDC-PERPETUAL", nullptr,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        window.findChild<QLineEdit*>("clientId")->setText("fake");
        window.findChild<QLineEdit*>("clientSecret")->setText("secret");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_COMPARE_WITH_TIMEOUT(
            window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
        QTest::qWait(850);
        QCOMPARE(tradingCalls, 0);
        QVERIFY(!window.findChild<QPushButton*>("start")->isEnabled());
        Engine restored;
        {
            Store db(path);
            QVERIFY(db.load(restored));
        }
        QVERIFY(restored.positionBlocked);
        QCOMPARE(restored.trackedPosition(), Int(1200000));
        // Only the explicitly confirmed reset discards inventory that was closed manually.
        QVERIFY(window.findChild<QPushButton*>("resetHistory")->isEnabled());
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                dialog->button(QMessageBox::Yes)->click();
        });
        window.findChild<QPushButton*>("resetHistory")->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            Store db(path);
            Engine cleared;
            db.load(cleared);
            return cleared.cycles().empty() && !cleared.positionBlocked;
        })(),
            5000);
        QCOMPARE(tradingCalls, 0);
        window.close();
    }

    void offlineTakesCountOnlyMissedFills_data()
    {
        QTest::addColumn<bool>("isLong");
        QTest::addColumn<bool>("full");
        QTest::newRow("long-partial") << true << false;
        QTest::newRow("long-full") << true << true;
        QTest::newRow("short-partial") << false << false;
        QTest::newRow("short-full") << false << true;
    }

    void offlineTakesCountOnlyMissedFills()
    {
        QFETCH(bool, isLong);
        QFETCH(bool, full);
        QTemporaryDir dir;
        const auto path = dir.filePath("overnight.db");
        Engine seed;
        seed.profileId = "overnight";
        seed.spec = {Scale / 10, Scale / 10000, {}};
        seed.externalPosition = 0;
        seed.nextCycle = 2;
        seed.nextOrder = 3;
        seed.importCycle({1, 1, isLong ? Side::Long : Side::Short, (isLong ? 100 : 110) * Scale,
            (isLong ? 110 : 100) * Scale, 4, 3, false});
        Order entry;
        entry.label = "dg_overnight_1_e_1";
        entry.exchangeId = "entry";
        entry.cycle = 1;
        entry.price = seed.cycles()[0].entry;
        entry.total = entry.filled = entry.desiredTotal = 4;
        entry.status = Status::Terminal;
        seed.importOrder(entry);
        Order take = entry;
        take.label = "dg_overnight_1_t_2";
        take.exchangeId = "take";
        take.role = Role::Take;
        take.price = seed.cycles()[0].take;
        take.filled = 1;
        take.status = Status::Open;
        seed.importOrder(take);
        {
            Store db(path);
            db.save(seed);
        }
        QJsonObject current{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_type", "limit"},
            {"direction", isLong ? "sell" : "buy"}, {"label", QString::fromStdString(take.label)},
            {"order_id", "take"}, {"price", isLong ? 110.0 : 100.0}, {"amount", 0.0004},
            {"filled_amount", full ? 0.0004 : 0.0003}, {"order_state", full ? "filled" : "open"},
            {"last_update_timestamp", 10}};
        const auto trade = [](const QString& id, double amount, double fee) {
            return QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_id", "take"},
                {"trade_id", id}, {"timestamp", 2}, {"amount", amount}, {"fee", fee},
                {"fee_currency", "USDC"}};
        };
        QJsonArray trades{
            trade("already-counted", 0.0001, 1000), trade("overnight-a", 0.0002, 0.02)};
        if (full)
            trades.append(trade("overnight-b", 0.0001, -0.001));
        bool feesAvailable = false;
        int tradingCalls = 0, feeReads = 0;
        QWebSocket* peer = nullptr;
        QWebSocketServer server("Offline takes", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            peer = socket;
            connect(
                socket, &QWebSocket::textMessageReceived, this, [&, socket](const QString& frame) {
                    const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                    const auto method = j["method"].toString();
                    QJsonValue result = QJsonObject{};
                    if (method == "public/auth")
                        result = QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}};
                    else if (method == "public/get_instrument")
                        result = QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                            {"base_currency", "BTC"}, {"settlement_currency", "USDC"},
                            {"kind", "future"}, {"settlement_period", "perpetual"},
                            {"state", "open"}, {"tick_size", 0.1}, {"min_trade_amount", 0.0001},
                            {"contract_size", 0.0001}};
                    else if (method == "private/get_account_summary")
                        result = QJsonObject{{"id", 7654321}};
                    else if (method == "public/ticker")
                        result = QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                            {"best_bid_price", 104.9}, {"best_ask_price", 105.1}};
                    else if (method == "private/get_position")
                        result = QJsonObject{{"size_currency",
                            (isLong ? 1 : -1) * (0.0004 - current["filled_amount"].toDouble())}};
                    else if (method == "private/get_open_orders_by_instrument")
                        result =
                            current["order_state"] == "open" ? QJsonArray{current} : QJsonArray{};
                    else if (method == "private/get_order_state")
                        result = current;
                    else if (method == "private/get_user_trades_by_order") {
                        ++feeReads;
                        QJsonArray history;
                        for (const auto& value : trades) {
                            auto t = value.toObject();
                            if (!feesAvailable)
                                t.remove("fee");
                            history.append(t);
                        }
                        result = history;
                    } else if (method == "private/buy" || method == "private/sell" ||
                               method == "private/edit" || method == "private/cancel")
                        ++tradingCalls;
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(
                        QJsonObject{{"jsonrpc", "2.0"}, {"id", j["id"]}, {"result", result}})
                            .toJson()));
                });
        });
        const auto endpoint = QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort()));
        const QString zero =
            "Approx. P&L: 0.0000 USDC\nFees (entry + take): 0.0000 USDC\nClosed trades: 0";
        const auto connectWindow = [](Window& window) {
            window.findChild<QLineEdit*>("clientId")->setText("fake");
            window.findChild<QLineEdit*>("clientSecret")->setText("fake");
            window.findChild<QPushButton*>("connect")->click();
        };
        {
            Window window(path, "BTC_USDC-PERPETUAL", nullptr, endpoint);
            connectWindow(window);
            auto* stats = window.findChild<QLabel*>("sessionStats");
            QVERIFY(stats);
            const QString prefix = QString("Approx. P&L: %1 USDC\nFees (entry + take): ")
                                       .arg(full ? "0.0030" : "0.0020");
            const QString suffix = QString("\nClosed trades: %1").arg(full ? 1 : 0);
            QTRY_COMPARE_WITH_TIMEOUT(
                stats->text(), prefix + "0.0000 USDC (incomplete)" + suffix, 5000);
            QTRY_VERIFY(feeReads > 0);
            {
                Store db(path);
                Engine restored;
                QVERIFY(db.load(restored));
                QCOMPARE(restored.cycles()[0].held, full ? Int(0) : Int(1));
            }
            feesAvailable = true;
            window.findChild<QPushButton*>("connect")->click();
            const QString expected = prefix + (full ? "0.0190" : "0.0200") + " USDC" + suffix;
            QTRY_COMPARE_WITH_TIMEOUT(stats->text(), expected, 5000);
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(
                QJsonObject{{"method", "subscription"},
                    {"params", QJsonObject{{"channel", "user.changes.BTC_USDC-PERPETUAL.raw"},
                                   {"data", QJsonObject{{"orders", QJsonArray{current}},
                                                {"trades", trades}}}}}})
                    .toJson()));
            QTest::qWait(350);
            QCOMPARE(stats->text(), expected);
            window.findChild<QPushButton*>("resetStats")->click();
            QCOMPARE(stats->text(), zero);
            window.findChild<QPushButton*>("connect")->click();
            QTRY_COMPARE_WITH_TIMEOUT(
                window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
            QCOMPARE(stats->text(), zero);
            window.close();
        }
        {
            Window restarted(path, "BTC_USDC-PERPETUAL", nullptr, endpoint);
            connectWindow(restarted);
            QTRY_COMPARE_WITH_TIMEOUT(restarted.findChild<QLabel*>("connectionStatus")->text(),
                QString("Connected"), 5000);
            QCOMPARE(restarted.findChild<QLabel*>("sessionStats")->text(), zero);
        }
        QCOMPARE(tradingCalls, 0);
    }

    void pauseProtectsAFillRacingCancellation_data()
    {
        QTest::addColumn<bool>("recover");
        QTest::addColumn<bool>("isLong");
        QTest::addColumn<bool>("resetStats");
        QTest::newRow("live-partial-short") << false << false << false;
        QTest::newRow("accepted-short-with-lost-reply") << true << false << false;
        QTest::newRow("live-partial-long") << false << true << false;
        QTest::newRow("accepted-long-with-lost-reply") << true << true << false;
        QTest::newRow("reset-partial-short") << false << false << true;
        QTest::newRow("reset-recovered-long") << true << true << true;
    }

    void pauseProtectsAFillRacingCancellation()
    {
        QFETCH(bool, recover);
        QFETCH(bool, isLong);
        QFETCH(bool, resetStats);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("profile.db");
        Engine seed;
        seed.profileId = "windowtest";
        if (isLong)
            seed.config.longs = {80000 * Scale, 84500 * Scale, 850 * Dollars, 1};
        else
            seed.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 1};
        if (recover) {
            seed.spec = {Scale / 10, Scale / 10000, {}};
            seed.quote(84600 * Scale - Scale / 10, 84600 * Scale + Scale / 10);
            seed.requestRebuild(seed.config);
            QVERIFY(seed.nextAction());
        }
        {
            Store db(path);
            db.save(seed);
        }
        QWebSocketServer server("Fake Deribit", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QJsonObject entry, take;
        int entries = 0, takes = 0, cancellations = 0, edits = 0;
        bool limitsOnly = true;
        QJsonArray entryFees, takeFees;
        const auto trade = [](const QString& order, const QString& id, double quantity,
                               double fee) {
            return QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_id", order},
                {"trade_id", id}, {"timestamp", QDateTime::currentMSecsSinceEpoch()},
                {"amount", quantity}, {"fee", fee}, {"fee_currency", "USDC"}};
        };
        QWebSocket* peer = nullptr;
        if (recover) {
            const auto& o = seed.orders()[0];
            entry = QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"}, {"order_type", "limit"},
                {"order_state", "open"}, {"direction", isLong ? "buy" : "sell"},
                {"label", QString::fromStdString(o.label)}, {"order_id", "entry-1"},
                {"price", static_cast<double>(o.price) / Scale},
                {"amount", static_cast<double>(o.total) / 10000}, {"filled_amount", 0.0003},
                {"last_update_timestamp", 1}};
            auto historical = trade("entry-1", "old-entry", 0.0003, 0.01);
            historical["timestamp"] = 1;
            entryFees.append(historical);
        }
        const auto response = [](QWebSocket* socket, qint64 id, const QJsonValue& result) {
            socket->sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})
                    .toJson()));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            peer = socket;
            connect(
                socket, &QWebSocket::textMessageReceived, this, [&, socket](const QString& frame) {
                    const auto j = QJsonDocument::fromJson(frame.toUtf8()).object();
                    const auto method = j["method"].toString();
                    const auto p = j["params"].toObject();
                    const auto id = j["id"].toInteger();
                    if (method == "public/auth")
                        response(socket, id,
                            QJsonObject{{"expires_in", 3600}, {"refresh_token", "fake"}});
                    else if (method == "public/get_instrument")
                        response(socket, id,
                            QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                                {"base_currency", "BTC"}, {"settlement_currency", "USDC"},
                                {"kind", "future"}, {"settlement_period", "perpetual"},
                                {"state", "open"}, {"tick_size", 0.1}, {"min_trade_amount", 0.0001},
                                {"contract_size", 0.0001}});
                    else if (method == "private/get_account_summary")
                        response(socket, id, QJsonObject{{"id", 987654321}});
                    else if (method == "private/get_position")
                        response(socket, id,
                            QJsonObject{{"size_currency",
                                (recover ? 0.0 : -0.0004) +
                                    (isLong ? 1 : -1) * (entry["filled_amount"].toDouble() -
                                                            take["filled_amount"].toDouble())}});
                    else if (method == "public/ticker")
                        response(socket, id,
                            QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                                {"best_bid_price", 84599.9}, {"best_ask_price", 84600.1}});
                    else if (method == "private/get_open_orders_by_instrument")
                        response(socket, id, QJsonArray{});
                    else if (method == "private/get_order_state_by_label")
                        response(socket, id, QJsonArray{entry});
                    else if (method == "private/get_order_state")
                        response(socket, id, p["order_id"] == "take-1" ? take : entry);
                    else if (method == "private/get_user_trades_by_order")
                        response(socket, id, p["order_id"] == "take-1" ? takeFees : entryFees);
                    else if (method == "private/buy" || method == "private/sell") {
                        limitsOnly = limitsOnly && p["type"] == "limit" &&
                                     p["post_only"] == false &&
                                     p["time_in_force"] == "good_til_cancelled";
                        const bool buy = method == "private/buy";
                        const bool isTake = buy != isLong;
                        auto& o = isTake ? take : entry;
                        o = QJsonObject{{"instrument_name", "BTC_USDC-PERPETUAL"},
                            {"order_type", "limit"}, {"order_state", "open"},
                            {"direction", buy ? "buy" : "sell"}, {"label", p["label"]},
                            {"order_id", isTake ? "take-1" : "entry-1"}, {"price", p["price"]},
                            {"amount", p["amount"]}, {"filled_amount", isTake ? 0.0 : 0.0003},
                            {"last_update_timestamp", 1}};
                        if (isTake)
                            ++takes;
                        else {
                            ++entries;
                            entryFees.append(trade("entry-1", "entry-a", 0.0003, 0.01));
                        }
                        response(socket, id,
                            QJsonObject{
                                {"order", o}, {"trades", isTake ? QJsonArray{} : entryFees}});
                    } else if (method == "private/cancel") {
                        ++cancellations;
                        entry["order_state"] = "cancelled";
                        entry["filled_amount"] = 0.0004;
                        entry["last_update_timestamp"] = 2;
                        entryFees.append(trade("entry-1", "entry-b", 0.0001, 0.003));
                        response(socket, id, entry);
                    } else if (method == "private/edit") {
                        ++edits;
                        take["amount"] = p["amount"];
                        take["last_update_timestamp"] = 3;
                        response(socket, id, QJsonObject{{"order", take}});
                    } else
                        response(socket, id, QJsonObject{{"ok", true}});
                });
        });
        Window window(path, "BTC_USDC-PERPETUAL", nullptr,
            QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Window(path, "BTC_USDC-PERPETUAL"));
        window.findChild<QLineEdit*>("clientId")->setText("fake-id");
        window.findChild<QLineEdit*>("clientSecret")->setText("fake-secret");
        window.findChild<QPushButton*>("connect")->click();
        QTRY_COMPARE_WITH_TIMEOUT(
            window.findChild<QLabel*>("connectionStatus")->text(), QString("Connected"), 5000);
        if (!recover) {
            QTRY_VERIFY(window.findChild<QPushButton*>("start")->isEnabled());
            window.findChild<QPushButton*>("start")->click();
            QTRY_COMPARE_WITH_TIMEOUT(takes, 1, 5000);
            QCOMPARE(entries, 1);
            window.findChild<QPushButton*>("pause")->click();
            QTRY_COMPARE_WITH_TIMEOUT(edits, 1, 5000);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(takes, 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(edits, 1, 5000);
        }
        QVERIFY(limitsOnly);
        QCOMPARE(cancellations, 1);
        QCOMPARE(take["amount"].toDouble(), 0.0004);
        QTest::qWait(100);
        Engine restored;
        {
            Store db(path);
            QVERIFY(db.load(restored));
        }
        QCOMPARE(restored.cycles()[0].held, Int(4));
        QVERIFY(restored.settled());
        QVERIFY(!restored.running);
        QCOMPARE(takes, 1);
        QCOMPARE(entries, recover ? 0 : 1);
        auto* stats = window.findChild<QLabel*>("sessionStats");
        QVERIFY(stats);
        QVERIFY(!window.findChild<QPushButton*>("resetHistory")->isEnabled());
        const QString entryCost = "0.0130";
        QTRY_COMPARE(stats->text(),
            QString("Approx. P&L: 0.0000 USDC\nFees (entry + take): %1 USDC\nClosed trades: 0")
                .arg(entryCost));
        const auto publish = [&](const QJsonObject& order,
                                 const QJsonArray& trades = QJsonArray{}) {
            const QJsonObject data{
                {"orders", order.isEmpty() ? QJsonArray{} : QJsonArray{order}}, {"trades", trades}};
            const QJsonObject params{
                {"channel", "user.changes.BTC_USDC-PERPETUAL.100ms"}, {"data", data}};
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonObject{{"method", "subscription"}, {"params", params}})
                    .toJson()));
        };
        // Response/subscription duplicates and manual trades do not add fees again.
        auto manual = trade("manual-order", "manual", 0.0001, 42.0);
        auto duplicateFees = entryFees;
        duplicateFees.append(manual);
        publish({}, duplicateFees);
        take["filled_amount"] = 0.0001;
        take["last_update_timestamp"] = 4;
        takeFees.append(trade("take-1", "take-a", 0.0001, -0.001));
        const auto partial = take;
        publish(partial, takeFees);
        publish(partial, takeFees);
        const QString partialPnl = isLong ? "0.4500" : "0.5400";
        const QString partialCost = "0.0120";
        QTRY_COMPARE(stats->text(),
            QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 0")
                .arg(partialPnl, partialCost));
        const QString zeroStats =
            "Approx. P&L: 0.0000 USDC\nFees (entry + take): 0.0000 USDC\nClosed trades: 0";
        if (resetStats) {
            Engine before;
            {
                Store db(path);
                QVERIFY(db.load(before));
            }
            auto* reset = window.findChild<QPushButton*>("resetStats");
            QVERIFY(reset);
            reset->click();
            QCOMPARE(stats->text(), zeroStats);
            Engine after;
            {
                Store db(path);
                QVERIFY(db.load(after));
            }
            QCOMPARE(encodeEngine(after), encodeEngine(before));
            publish(partial, duplicateFees);
            publish(partial, takeFees);
            QTest::qWait(100);
            QCOMPARE(stats->text(), zeroStats);
        }
        take["filled_amount"] = 0.0004;
        take["last_update_timestamp"] = 5;
        take["order_state"] = "filled";
        auto finalTrade = trade("take-1", "take-b", 0.0003, 0.006);
        auto missingFee = finalTrade;
        missingFee.remove("fee");
        takeFees.append(missingFee);
        publish(take, takeFees);
        publish(take, takeFees);
        publish(partial);
        const QString fullPnl =
            resetStats ? (isLong ? "1.3500" : "1.6200") : (isLong ? "1.8000" : "2.1600");
        QTRY_COMPARE(stats->text(),
            QString(
                "Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC (incomplete)\nClosed trades: 1")
                .arg(fullPnl, resetStats ? "0.0000" : partialCost));
        auto wrongCurrency = finalTrade;
        wrongCurrency["fee_currency"] = "BTC";
        publish({}, QJsonArray{wrongCurrency});
        QTest::qWait(350);
        QVERIFY(stats->text().contains("(incomplete)"));
        // A reconnect obtains the missed fee once, including for an already closed order.
        takeFees.replace(1, finalTrade);
        window.findChild<QPushButton*>("connect")->click();
        const QString fullCost = resetStats ? "0.0060" : "0.0180";
        QTRY_COMPARE_WITH_TIMEOUT(stats->text(),
            QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 1")
                .arg(fullPnl, fullCost),
            5000);
        publish({}, takeFees);
        publish(partial, entryFees);
        QTest::qWait(100);
        QCOMPARE(stats->text(),
            QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 1")
                .arg(fullPnl, fullCost));
        // Clear log leaves the durable inventory and counters intact.
        window.findChild<QPushButton*>("clearLog")->click();
        QVERIFY(window.findChild<QPlainTextEdit*>()->toPlainText().isEmpty());
        const auto nextOrder = restored.nextOrder;
        QTRY_VERIFY(window.findChild<QPushButton*>("resetHistory")->isEnabled());
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                dialog->button(QMessageBox::Yes)->click();
        });
        window.findChild<QPushButton*>("resetHistory")->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            Store db(path);
            Engine cleared;
            db.load(cleared);
            return cleared.cycles().empty() && cleared.orders().empty();
        })(),
            5000);
        {
            Store db(path);
            QVERIFY(db.load(restored));
        }
        QVERIFY(restored.externalPosition.has_value());
        QCOMPARE(*restored.externalPosition, recover ? Int(0) : Int(-40000));
        QVERIFY(restored.nextOrder >= nextOrder);
        QVERIFY(!restored.running);
        QCOMPARE(stats->text(),
            QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC\nClosed trades: 1")
                .arg(fullPnl, fullCost));
        window.findChild<QPushButton*>("resetStats")->click();
        QCOMPARE(stats->text(), zeroStats);
        publish(take, takeFees);
        publish(take, entryFees);
        QTest::qWait(350);
        QCOMPARE(stats->text(), zeroStats);
        window.close();
        // Session counters are deliberately absent from the saved state.
        {
            Store db(path);
            QVERIFY(db.load(restored));
        }
        QVERIFY(!encodeEngine(restored).contains("sessionPnl"));
        QVERIFY(!encodeEngine(restored).contains("sessionFees"));
    }
};
QTEST_MAIN(WindowTests)
#include "window_tests.moc"
