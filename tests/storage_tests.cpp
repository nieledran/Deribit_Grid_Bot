#include "storage/store.h"
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

    void transitionSurvivesInterruptedCancellation()
    {
        Engine e;
        e.profileId = "transition";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.longs = {82500 * Scale, 84500 * Scale, 850 * Dollars, 3};
        e.importCycle({e.nextCycle++, 0, Side::Long, 83700 * Scale, 83800 * Scale, 2, 2, true});
        e.quote(83500 * Scale, 83500 * Scale);
        const auto finish = [](Engine& engine, const Action& a) {
            const auto o = *engine.order(a.label);
            engine.apply({o.label, o.exchangeId.empty() ? "ex-" + o.label : o.exchangeId, o.total,
                o.filled, o.exchangeTime + 1, o.price, a.kind == Mutation::Cancel});
            engine.resolved(a.label);
        };
        auto take = e.nextAction();
        QVERIFY(take && take->role == Role::Take);
        finish(e, *take);
        e.requestRebuild(e.config);
        while (const auto a = e.nextAction())
            finish(e, *a);
        QCOMPARE(e.lattice.size(), size_t(2));
        QCOMPARE(e.rebuildFloor[0], Int(2));
        auto restored = decodeEngine(encodeEngine(e));
        restored.quote(83500 * Scale, 83500 * Scale);
        QCOMPARE(restored.transitionSlots(Side::Long), 2);
        QVERIFY(restored.transitioning(Side::Long));
        const auto old = *restored.order(take->label);
        restored.apply({old.label, old.exchangeId, old.total, old.total, 20, old.price, true});
        auto cancel = restored.nextAction();
        QVERIFY(cancel && cancel->kind == Mutation::Cancel);
        finish(restored, *cancel);
        // Restart between cancellation replies, with no duplicate list of old cycles saved.
        restored = decodeEngine(encodeEngine(restored));
        restored.quote(83490 * Scale, 83490 * Scale);
        while (const auto a = restored.nextAction())
            finish(restored, *a);
        QCOMPARE(restored.rebuildFloor[0], Int(2));
        QCOMPARE(restored.lattice.size(), size_t(3));
        QCOMPARE(restored.lattice.back().entry, 82500 * Scale);
        QVERIFY(!restored.transitioning(Side::Long));
        // Existing profiles have no cohort marker and remain readable.
        auto legacy = encodeEngine(restored);
        legacy["version"] = 4;
        QCOMPARE(decodeEngine(legacy).rebuildFloor[0], Int(1));
        legacy["rebuildFloor"] = 2;
        QCOMPARE(decodeEngine(legacy).rebuildFloor[0], Int(2));
        QCOMPARE(decodeEngine(legacy).rebuildFloor[1], Int(2));
        auto invalid = encodeEngine(restored);
        invalid["rebuildLongFloor"] = restored.nextCycle + 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(invalid));
    }

    void selectiveRebuildSurvivesRestart_data()
    {
        QTest::addColumn<bool>("longSide");
        QTest::addColumn<bool>("racedFill");
        QTest::newRow("Long-pending") << true << false;
        QTest::newRow("Short-pending") << false << false;
        QTest::newRow("Long-raced-fill") << true << true;
        QTest::newRow("Short-raced-fill") << false << true;
    }

    void selectiveRebuildSurvivesRestart()
    {
        QFETCH(bool, longSide);
        QFETCH(bool, racedFill);
        QTemporaryDir dir;
        Store store(dir.filePath("selective.db"));
        Engine e;
        e.profileId = "selective";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.longs = {80000 * Scale, 84500 * Scale, 850 * Dollars, 3};
        e.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 3};
        e.quote(84600 * Scale, 84600 * Scale);
        e.requestRebuild(e.config);
        const auto finish = [](Engine& engine, const Action& a, Int fill = 0) {
            auto o = *engine.order(a.label);
            if (a.kind == Mutation::Edit)
                o.total = a.total;
            engine.apply({o.label, o.exchangeId.empty() ? "ex-" + o.label : o.exchangeId, o.total,
                std::max(o.filled, fill), o.exchangeTime + 1, o.price, a.kind == Mutation::Cancel});
            engine.resolved(a.label);
        };
        while (const auto a = e.nextAction())
            finish(e, *a);
        const auto side = longSide ? Side::Long : Side::Short,
                   other = longSide ? Side::Short : Side::Long;
        const size_t index = static_cast<size_t>(side), otherIndex = static_cast<size_t>(other);
        const auto originals = e.orders();
        const auto floors = e.rebuildFloor;
        const auto anchors = e.anchors;
        auto next = e.config;
        (longSide ? next.longs : next.shorts).levels = 4;
        QCOMPARE(e.requestRebuild(next, true), longSide ? 1u : 2u);
        const auto pending = e.nextAction();
        QVERIFY(pending && pending->kind == Mutation::Cancel);
        if (racedFill)
            finish(e, *pending, 1);
        const auto actual = e.orders();
        e.disconnected();
        store.save(e);
        Engine restored;
        QVERIFY(store.load(restored));
        QVERIFY(restored.pendingConfig);
        QCOMPARE(restored.rebuildSides, longSide ? 1u : 2u);
        QCOMPARE(restored.rebuildFloor[otherIndex], floors[otherIndex]);
        QCOMPARE(restored.anchors, anchors);
        QVERIFY(!restored.nextAction());
        restored.quote(84600 * Scale, 84600 * Scale);
        // The exchange snapshots resolve all uncertain orders before rebuilding.
        for (const auto& o : actual)
            if (restored.order(o.label)->status == Status::Uncertain) {
                restored.apply({o.label, o.exchangeId, o.total, o.filled, o.exchangeTime + 1,
                    o.price, o.status == Status::Terminal});
                restored.resolved(o.label);
            }
        int cancels = 0, creates = 0, takes = 0;
        while (const auto a = restored.nextAction()) {
            QCOMPARE(restored.cycle(restored.order(a->label)->cycle)->side, side);
            cancels += a->kind == Mutation::Cancel;
            creates += a->kind == Mutation::Create && a->role == Role::Entry;
            takes += a->role == Role::Take;
            finish(restored, *a);
            store.save(restored);
        }
        QCOMPARE(cancels, racedFill ? 2 : 3);
        QCOMPARE(creates, racedFill ? 3 : 4);
        QCOMPARE(takes, racedFill ? 1 : 0);
        for (const auto& o : originals)
            if (restored.cycle(o.cycle)->side == other) {
                QCOMPARE(restored.order(o.label)->exchangeId, o.exchangeId);
                QCOMPARE(restored.order(o.label)->total, o.total);
                QCOMPARE(restored.order(o.label)->status, Status::Open);
                QVERIFY(!restored.cycle(o.cycle)->retired);
            }
        QVERIFY(!restored.rebuilding);
        QCOMPARE(restored.rebuildSides, 0u);
        store.save(restored);
        QVERIFY(store.load(e));
        QCOMPARE(e.rebuildFloor[index], Int(7));
        QCOMPARE(e.rebuildFloor[otherIndex], floors[otherIndex]);
        QCOMPARE(e.anchors[otherIndex], anchors[otherIndex]);
        QCOMPARE(e.transitionSlots(side), racedFill ? 3 : 4);
        auto invalid = encodeEngine(e);
        invalid["rebuildSides"] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(invalid));
        invalid = encodeEngine(e);
        invalid["longAnchor"] = -1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(invalid));
    }

    void constructorFailureReleasesConnection()
    {
        const auto before = QSqlDatabase::connectionNames();
        QTemporaryDir dir;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Store(dir.filePath("missing/invalid.db")));
        QCOMPARE(QSqlDatabase::connectionNames(), before);
    }

    void nativeIntegersRejectFractionsAndOverflow()
    {
        Engine e;
        e.profileId = "typed";
        auto json = encodeEngine(e);
        QCOMPARE(json["version"].toInt(), 5);
        QVERIFY(json["nextOrder"].isDouble());
        json["nextOrder"] = "1";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e);
        json["nextOrder"] = 1.5;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e);
        json["nextOrder"] = 1e30;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e);
        json["running"] = "false";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e);
        json["cycles"] = QJsonObject{};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e);
        json["nextOrder"] = QJsonValue(qlonglong(INT64_MAX));
        QCOMPARE(
            decodeEngine(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object()).nextOrder,
            Int(INT64_MAX));
        // Old profiles stored decimal strings. The legacy adapter accepts all supported versions.
        for (int version : {1, 2, 3}) {
            auto old = encodeEngine(e);
            old["version"] = version;
            for (const auto* key : {"priceTick", "quantityStep", "generation", "nextCycle",
                     "nextOrder", "historyFloor"})
                old[key] = QString::number(old[key].toInteger());
            auto config = old["config"].toObject();
            for (const auto* side : {"long", "short"}) {
                auto band = config[side].toObject();
                for (const auto* key : {"low", "high", "budget"})
                    band[key] = QString::number(band[key].toInteger());
                config[side] = band;
            }
            old["config"] = config;
            QCOMPARE(decodeEngine(old).nextOrder, Int(1));
        }
    }

    void archiveIsAtomicAndLateFillsAreCountedOnce()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("archive.db");
        Engine e;
        e.profileId = "archived";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.shorts = {80000 * Scale, 90000 * Scale, 850 * Dollars, 14};
        for (int i = 0; i < 1100; ++i) {
            const Int id = e.nextCycle++;
            e.importCycle({id, 0, Side::Short, 82500 * Scale, 82400 * Scale, 2, 0, true});
            Order order;
            order.label =
                "dg_archived_" + std::to_string(id) + "_e_" + std::to_string(e.nextOrder++);
            order.exchangeId = "exchange-" + std::to_string(id);
            order.cycle = id;
            order.price = 82500 * Scale;
            order.total = 2;
            order.status = Status::Terminal;
            e.importOrder(order);
        }
        Store store(path);
        store.save(e);
        QCOMPARE(e.orders().size(), size_t(1000));
        QCOMPARE(e.cycles().size(), size_t(1000));
        QVERIFY(store.recoverOrder("dg_archived_1_e_1", e));
        auto order = *e.order("dg_archived_1_e_1");
        e.apply({order.label, order.exchangeId, 2, 1, 2, order.price, true});
        e.apply({order.label, order.exchangeId, 2, 1, 2, order.price, true});
        QCOMPARE(e.cycle(1)->held, Int(1));
        QCOMPARE(e.availableSlots(Side::Short), 13);
        auto take = e.nextAction();
        QVERIFY(take);
        QCOMPARE(take->role, Role::Take);
        QCOMPARE(take->total, Int(1));
        store.save(e);
        QVERIFY(e.cycle(1));
        QCOMPARE(e.orders().size(), size_t(1001));
        Engine restored;
        QVERIFY(store.load(restored));
        QCOMPARE(restored.cycle(1)->held, Int(1));
        QVERIFY(store.ownsExchangeId("exchange-2"));
        Engine reset;
        reset.profileId = e.profileId;
        reset.spec = e.spec;
        reset.nextCycle = e.nextCycle;
        reset.nextOrder = e.nextOrder;
        reset.clearHistory(0);
        QVERIFY(!store.recoverOrder("dg_archived_2_e_2", reset));
        QVERIFY(reset.cycles().empty());
        QVERIFY(store.feesComplete());
        store.expectFee("exchange-1", 1);
        QVERIFY(!store.feesComplete());
        QVERIFY(store.recordFee("exchange-1/trade-1", "exchange-1", 1));
        QVERIFY(!store.recordFee("exchange-1/trade-1", "exchange-1", 1));
        QVERIFY(store.feesComplete());
        store.expectFee("exchange-1", 2);
        QVERIFY(!store.feesComplete());
        store.resetFees();
        QVERIFY(store.feesComplete());
        QVERIFY(store.missingFees().isEmpty());
        QVERIFY(!store.recordFee("exchange-1/trade-1", "exchange-1", 1));
        store.expectFee("exchange-1", 1);
        QVERIFY(store.recordFee("exchange-1/trade-2", "exchange-1", 1));
        QVERIFY(store.feesComplete());
        // Fee deduplication belongs only to this Store session.
        {
            Store another(path);
            QVERIFY(another.recordFee("exchange-1/trade-1", "exchange-1", 1));
        }
    }

    void failedArchiveTransactionDoesNotEvictMemory()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("rollback.db");
        Engine e;
        e.profileId = "rollback";
        e.spec = {Scale / 10, Scale / 10000, {}};
        Store store(path);
        store.save(e);
        const auto connection = QString("failure-injection");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_archive BEFORE INSERT ON archive BEGIN SELECT "
                           "RAISE(ABORT,'injected archive failure'); END"));
        }
        QSqlDatabase::removeDatabase(connection);
        for (int i = 0; i < 1001; ++i) {
            const Int id = e.nextCycle++;
            e.importCycle({id, 0, Side::Short, 82500 * Scale, 82400 * Scale, 1, 0, true});
            Order o;
            o.label = "dg_rollback_" + std::to_string(id) + "_e_" + std::to_string(e.nextOrder++);
            o.cycle = id;
            o.price = 82500 * Scale;
            o.total = 1;
            o.status = Status::Terminal;
            e.importOrder(o);
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.save(e));
        QCOMPARE(e.orders().size(), size_t(1001));
        Engine restored;
        QVERIFY(store.load(restored));
        QVERIFY(restored.orders().empty());
        QVERIFY(!store.recoverOrder("dg_rollback_1_e_1", restored));
    }

    void durablePlacementAndPartialFills()
    {
        try {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("state.db");
            Engine e;
            e.profileId = "durable";
            e.spec = {Scale / 10, Scale / 10000, {}};
            e.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 1};
            e.quote(84600 * Scale - Scale / 10, 84600 * Scale + Scale / 10);
            e.requestRebuild(e.config);
            auto a = e.nextAction();
            QVERIFY(a);
            QVERIFY(a->kind == Mutation::Create);
            {
                Store db(path);
                db.save(e);
                db.bindAccount("42/test/BTC_USDC-PERPETUAL");
            }
            Engine restored;
            {
                Store db(path);
                QVERIFY(db.load(restored));
                db.bindAccount("42/test/BTC_USDC-PERPETUAL");
                QVERIFY_THROWS_EXCEPTION(
                    std::runtime_error, db.bindAccount("43/test/BTC_USDC-PERPETUAL"));
            }
            QCOMPARE(restored.orders().size(), size_t(1));
            QCOMPARE(restored.orders()[0].label, a->label);
            QVERIFY(!restored.nextAction());
            auto o = restored.orders()[0];
            restored.apply({o.label, "accepted-but-reply-lost", o.total, 3, 10, o.price, false});
            restored.resolved(o.label);
            auto take = restored.nextAction();
            QVERIFY(take);
            QCOMPARE(take->role, Role::Take);
            QCOMPARE(take->total, Int(3));
            {
                Store db(path);
                db.save(restored);
            }
            Engine second;
            {
                Store db(path);
                QVERIFY(db.load(second));
            }
            QCOMPARE(second.cycles()[0].held, Int(3));
            QCOMPARE(second.orders().size(), size_t(2));
            second.apply({o.label, "accepted-but-reply-lost", o.total, 3, 10, o.price, false});
            QCOMPARE(second.cycles()[0].held, Int(3));
            QVERIFY(!second.nextAction());
            QCOMPARE(encodeEngine(second), encodeEngine(restored));
        } catch (const std::exception& ex) {
            QFAIL(ex.what());
        }
    }

    void corruptedStateIsRejected()
    {
        Engine e;
        e.profileId = "valid";
        auto json = encodeEngine(e);
        json["nextOrder"] = "broken";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
        json = encodeEngine(e);
        json["version"] = 999;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeEngine(json));
    }

    void bothGridLevelsAndOldProfiles()
    {
        Engine e;
        e.profileId = "upgrade";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.longs = {80000 * Scale, 84500 * Scale, 850 * Dollars, 2};
        e.config.shorts = {84500 * Scale, 90000 * Scale, 850 * Dollars, 2};
        e.quote(84600 * Scale, 84600 * Scale);
        e.requestRebuild(e.config);
        QVERIFY(e.nextAction());
        auto json = encodeEngine(e);
        const auto restored = decodeEngine(json);
        QCOMPARE(encodeEngine(restored), json);
        QCOMPARE(restored.lattice.size(), size_t(4));
        QCOMPARE(restored.lattice[0].side, Side::Short);
        QCOMPARE(restored.lattice[2].side, Side::Long);
        // Version 1 stored only the active lattice and omitted its side.
        QJsonArray oldLevels;
        for (auto v : json["levels"].toArray()) {
            auto level = v.toObject();
            if (level["side"].toInt() != static_cast<int>(Side::Short))
                continue;
            level.remove("side");
            oldLevels.append(level);
        }
        json["levels"] = oldLevels;
        json["version"] = 1;
        const auto old = decodeEngine(json);
        QCOMPARE(old.lattice.size(), size_t(2));
        QCOMPARE(old.lattice[0].side, Side::Short);
        QCOMPARE(old.orders()[0].label, e.orders()[0].label);
        QCOMPARE(old.cycles()[0].held, e.cycles()[0].held);
    }

    void positionBaselineSurvivesRestart()
    {
        Engine e;
        e.profileId = "baseline";
        e.spec = {Scale / 10, Scale / 10000, {}};
        QVERIFY(e.reconcilePosition(-1200000));
        auto restored = decodeEngine(encodeEngine(e));
        QCOMPARE(restored.externalPosition, e.externalPosition);
        QCOMPARE(restored.trackedPosition(), Int(0));
        QVERIFY(!restored.reconcilePosition(0));
        restored = decodeEngine(encodeEngine(restored));
        QVERIFY(restored.positionBlocked);
        auto legacy = encodeEngine(e);
        legacy["version"] = 2;
        legacy.remove("externalPosition");
        legacy.remove("positionBlocked");
        QVERIFY(!decodeEngine(legacy).externalPosition.has_value());
    }
};
QTEST_GUILESS_MAIN(StorageTests)
#include "storage_tests.moc"
