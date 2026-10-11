#include "store.h"
#include <QTemporaryDir>
#include <QJsonArray>
#include <QtTest>
#include <stdexcept>

using namespace grid;
class StorageTests : public QObject {
    Q_OBJECT
private slots:
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
        QCOMPARE(restored.orders.size(), size_t(1)); QCOMPARE(restored.orders[0].label, a->label);
        QVERIFY(!restored.nextAction());
        auto o = restored.orders[0]; restored.apply({o.label, "accepted-but-reply-lost", o.total, 3, 10, o.price, false}); restored.resolved(o.label);
        auto take = restored.nextAction(); QVERIFY(take); QCOMPARE(take->role, Role::Take); QCOMPARE(take->total, Int(3));
        { Store db(path); db.save(restored); }
        Engine second;
        { Store db(path); QVERIFY(db.load(second)); }
        QCOMPARE(second.cycles[0].held, Int(3)); QCOMPARE(second.orders.size(), size_t(2));
        second.apply({o.label, "accepted-but-reply-lost", o.total, 3, 10, o.price, false});
        QCOMPARE(second.cycles[0].held, Int(3)); QVERIFY(!second.nextAction());
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
        QCOMPARE(old.orders[0].label, e.orders[0].label); QCOMPARE(old.cycles[0].held, e.cycles[0].held);
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
