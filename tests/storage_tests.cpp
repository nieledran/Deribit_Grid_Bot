#include "store.h"
#include <QTemporaryDir>
#include <QJsonArray>
#include <QtTest>
#include <stdexcept>
#include <QSqlQuery>
#include <QJsonDocument>

using namespace grid;
class StorageTests : public QObject {
    Q_OBJECT
private slots:
    void constructorFailureReleasesConnection() {
        const auto before = QSqlDatabase::connectionNames();
        QTemporaryDir dir;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Store(dir.filePath("missing/invalid.db")));
        QCOMPARE(QSqlDatabase::connectionNames(), before);
    }
    void nativeIntegersRejectFractionsAndOverflow() {
        Engine e; e.profileId = "typed";
        auto json = encodeEngine(e); QCOMPARE(json["version"].toInt(), 4);
        QVERIFY(json["nextOrder"].isDouble());
        json["nextOrder"] = "1"; QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e); json["nextOrder"] = 1.5; QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e); json["nextOrder"] = 1e30; QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e); json["running"] = "false"; QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e); json["cycles"] = QJsonObject{}; QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e); json["nextOrder"] = QJsonValue(qlonglong(INT64_MAX));
        QCOMPARE(decodeEngine(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object()).nextOrder, Int(INT64_MAX));
        // Old profiles stored decimal strings. The legacy adapter accepts all supported versions.
        for (int version : {1, 2, 3}) {
            auto old = encodeEngine(e); old["version"] = version;
            for (const auto* key : {"priceTick", "quantityStep", "generation", "nextCycle", "nextOrder", "historyFloor"}) old[key] = QString::number(old[key].toInteger());
            auto config = old["config"].toObject();
            for (const auto* side : {"long", "short"}) {
                auto band = config[side].toObject();
                for (const auto* key : {"low", "high", "budget"}) band[key] = QString::number(band[key].toInteger());
                config[side] = band;
            }
            old["config"] = config; QCOMPARE(decodeEngine(old).nextOrder, Int(1));
        }
    }
    void archiveIsAtomicAndLateFillsAreCountedOnce() {
        QTemporaryDir dir; const auto path = dir.filePath("archive.db");
        Engine e; e.profileId = "archived"; e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.shorts = {80000 * Scale, 90000 * Scale, 850 * Dollars, 14};
        for (int i = 0; i < 1100; ++i) {
            const Int id = e.nextCycle++; e.importCycle({id, 0, Side::Short, 82500 * Scale, 82400 * Scale, 2, 0, true});
            Order order; order.label = "dg_archived_" + std::to_string(id) + "_e_" + std::to_string(e.nextOrder++);
            order.exchangeId = "exchange-" + std::to_string(id); order.cycle = id; order.price = 82500 * Scale;
            order.total = 2; order.status = Status::Terminal; e.importOrder(order);
        }
        Store store(path); store.save(e); QCOMPARE(e.orders().size(), size_t(1000)); QCOMPARE(e.cycles().size(), size_t(1000));
        QVERIFY(store.recoverOrder("dg_archived_1_e_1", e));
        auto order = *e.order("dg_archived_1_e_1");
        e.apply({order.label, order.exchangeId, 2, 1, 2, order.price, true});
        e.apply({order.label, order.exchangeId, 2, 1, 2, order.price, true});
        QCOMPARE(e.cycle(1)->held, Int(1)); QCOMPARE(e.availableSlots(Side::Short), 13);
        auto take = e.nextAction(); QVERIFY(take); QCOMPARE(take->role, Role::Take); QCOMPARE(take->total, Int(1));
        store.save(e); QVERIFY(e.cycle(1)); QCOMPARE(e.orders().size(), size_t(1001));
        Engine restored; QVERIFY(store.load(restored)); QCOMPARE(restored.cycle(1)->held, Int(1));
        QVERIFY(store.ownsExchangeId("exchange-2"));
        Engine reset; reset.profileId = e.profileId; reset.spec = e.spec; reset.nextCycle = e.nextCycle; reset.nextOrder = e.nextOrder;
        reset.clearHistory(0); QVERIFY(!store.recoverOrder("dg_archived_2_e_2", reset)); QVERIFY(reset.cycles().empty());
        QVERIFY(store.feesComplete()); store.expectFee("exchange-1", 1); QVERIFY(!store.feesComplete());
        QVERIFY(store.recordFee("exchange-1/trade-1", "exchange-1", 1));
        QVERIFY(!store.recordFee("exchange-1/trade-1", "exchange-1", 1)); QVERIFY(store.feesComplete());
        // Fee deduplication belongs only to this Store session.
        { Store another(path); QVERIFY(another.recordFee("exchange-1/trade-1", "exchange-1", 1)); }
    }
    void failedArchiveTransactionDoesNotEvictMemory() {
        QTemporaryDir dir; const auto path = dir.filePath("rollback.db");
        Engine e; e.profileId = "rollback"; e.spec = {Scale / 10, Scale / 10000, {}};
        Store store(path); store.save(e);
        const auto connection = QString("failure-injection");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("CREATE TRIGGER reject_archive BEFORE INSERT ON archive BEGIN SELECT RAISE(ABORT,'injected archive failure'); END"));
        }
        QSqlDatabase::removeDatabase(connection);
        for (int i = 0; i < 1001; ++i) {
            const Int id = e.nextCycle++; e.importCycle({id, 0, Side::Short, 82500 * Scale, 82400 * Scale, 1, 0, true});
            Order o; o.label = "dg_rollback_" + std::to_string(id) + "_e_" + std::to_string(e.nextOrder++);
            o.cycle = id; o.price = 82500 * Scale; o.total = 1; o.status = Status::Terminal; e.importOrder(o);
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.save(e)); QCOMPARE(e.orders().size(), size_t(1001));
        Engine restored; QVERIFY(store.load(restored)); QVERIFY(restored.orders().empty());
        QVERIFY(!store.recoverOrder("dg_rollback_1_e_1", restored));
    }
    void durablePlacementAndPartialFills() {
        try {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("state.db");
        Engine e; e.profileId = "durable"; e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 1};
        e.quote(84600 * Scale - Scale / 10, 84600 * Scale + Scale / 10); e.requestRebuild(e.config);
        auto a = e.nextAction(); QVERIFY(a); QVERIFY(a->kind == Mutation::Create);
        { Store db(path); db.save(e); db.bindAccount("42/test/BTC_USDC-PERPETUAL"); }
        Engine restored;
        { Store db(path); QVERIFY(db.load(restored)); db.bindAccount("42/test/BTC_USDC-PERPETUAL"); QVERIFY_THROWS_EXCEPTION(std::runtime_error, db.bindAccount("43/test/BTC_USDC-PERPETUAL")); }
        QCOMPARE(restored.orders().size(), size_t(1)); QCOMPARE(restored.orders()[0].label, a->label);
        QVERIFY(!restored.nextAction());
        auto o = restored.orders()[0]; restored.apply({o.label, "accepted-but-reply-lost", o.total, 3, 10, o.price, false}); restored.resolved(o.label);
        auto take = restored.nextAction(); QVERIFY(take); QCOMPARE(take->role, Role::Take); QCOMPARE(take->total, Int(3));
        { Store db(path); db.save(restored); }
        Engine second;
        { Store db(path); QVERIFY(db.load(second)); }
        QCOMPARE(second.cycles()[0].held, Int(3)); QCOMPARE(second.orders().size(), size_t(2));
        second.apply({o.label, "accepted-but-reply-lost", o.total, 3, 10, o.price, false});
        QCOMPARE(second.cycles()[0].held, Int(3)); QVERIFY(!second.nextAction());
        QCOMPARE(encodeEngine(second), encodeEngine(restored));
        } catch (const std::exception& ex) { QFAIL(ex.what()); }
    }
    void corruptedStateIsRejected() {
        Engine e; e.profileId = "valid"; auto json = encodeEngine(e); json["nextOrder"] = "broken";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e); json["version"] = 999; QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
    }
    void bothGridLevelsAndOldProfiles() {
        Engine e; e.profileId = "upgrade"; e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.longs = {80000 * Scale, 84500 * Scale, 850 * Dollars, 2};
        e.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 2};
        e.quote(84600 * Scale, 84600 * Scale); e.requestRebuild(e.config); QVERIFY(e.nextAction());
        auto json = encodeEngine(e); const auto restored = decodeEngine(json);
        QCOMPARE(encodeEngine(restored), json); QCOMPARE(restored.lattice.size(), size_t(4));
        QCOMPARE(restored.lattice[0].side, Side::Short); QCOMPARE(restored.lattice[2].side, Side::Long);
        // Version 1 stored only the active lattice and omitted its side.
        QJsonArray oldLevels;
        for (auto v : json["levels"].toArray()) {
            auto level = v.toObject(); if (level["side"].toInt() != static_cast<int>(Side::Short)) continue;
            level.remove("side"); oldLevels.append(level);
        }
        json["levels"] = oldLevels; json["version"] = 1;
        const auto old = decodeEngine(json);
        QCOMPARE(old.lattice.size(), size_t(2)); QCOMPARE(old.lattice[0].side, Side::Short);
        QCOMPARE(old.orders()[0].label, e.orders()[0].label); QCOMPARE(old.cycles()[0].held, e.cycles()[0].held);
    }
    void positionBaselineSurvivesRestart() {
        Engine e; e.profileId = "baseline"; e.spec = {Scale / 10, Scale / 10000, {}};
        QVERIFY(e.reconcilePosition(-1200000));
        auto restored = decodeEngine(encodeEngine(e));
        QCOMPARE(restored.externalPosition, e.externalPosition); QCOMPARE(restored.trackedPosition(), Int(0));
        QVERIFY(!restored.reconcilePosition(0));
        restored = decodeEngine(encodeEngine(restored)); QVERIFY(restored.positionBlocked);
        auto legacy = encodeEngine(e); legacy["version"] = 2; legacy.remove("externalPosition"); legacy.remove("positionBlocked");
        QVERIFY(!decodeEngine(legacy).externalPosition.has_value());
    }
};
QTEST_GUILESS_MAIN(StorageTests)
#include "storage_tests.moc"
