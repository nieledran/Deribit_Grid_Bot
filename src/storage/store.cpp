#include "storage/store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>
#include <set>
#include <limits>
#include <algorithm>

using namespace grid;

namespace {
QJsonValue s(Int value)
{
    return QJsonValue(qint64(value));
}

Int n(const QJsonObject& o, const char* key)
{
    const auto value = o[key];
    if (value.isString()) {
        bool ok = false;
        const Int v = value.toString().toLongLong(&ok);
        if (ok)
            return v;
    } else if (value.isDouble()) {
        const auto sentinel = std::numeric_limits<qint64>::min();
        const Int v = value.toInteger(sentinel);
        if (v != sentinel || value == QJsonValue(sentinel))
            return v;
    }
    throw std::runtime_error(std::string("Invalid stored integer: ") + key);
}

QJsonObject encodeBand(const Band& b)
{
    return {{"low", s(b.low)}, {"high", s(b.high)}, {"budget", s(b.budget)}, {"levels", b.levels}};
}

int bounded(const QJsonObject& o, const char* key, int low, int high)
{
    const auto v = n(o, key);
    if (v < low || v > high)
        throw std::runtime_error(std::string("Invalid stored enum/range: ") + key);
    return static_cast<int>(v);
}

bool boolean(const QJsonObject& o, const char* key)
{
    if (!o[key].isBool())
        throw std::runtime_error(std::string("Invalid stored boolean: ") + key);
    return o[key].toBool();
}

Band decodeBand(const QJsonObject& o)
{
    return {n(o, "low"), n(o, "high"), n(o, "budget"), bounded(o, "levels", 1, 200)};
}

QJsonObject encodeConfig(const Config& c)
{
    return {{"instrument", QString::fromStdString(c.instrument)}, {"production", c.production},
        {"long", encodeBand(c.longs)}, {"short", encodeBand(c.shorts)}};
}

Config decodeConfig(const QJsonObject& o)
{
    Config c;
    c.instrument = o["instrument"].toString().toStdString();
    c.production = boolean(o, "production");
    c.longs = decodeBand(o["long"].toObject());
    c.shorts = decodeBand(o["short"].toObject());
    const auto error = Engine::validate(c);
    if (!error.empty())
        throw std::runtime_error(error);
    return c;
}

void validateNativeIntegers(const QJsonValue& value)
{
    if (value.isArray()) {
        for (const auto& item : value.toArray())
            validateNativeIntegers(item);
        return;
    }
    if (!value.isObject())
        return;
    static const std::set<QString> integers{"version", "low", "high", "budget", "levels", "id",
        "generation", "side", "entry", "take", "capacity", "held", "cycle", "role", "price",
        "total", "filled", "exchangeTime", "status", "mutation", "desiredTotal", "priceTick",
        "quantityStep", "tick", "nextCycle", "nextOrder", "historyFloor", "rebuildFloor",
        "rebuildLongFloor", "rebuildShortFloor", "rebuildSides", "longAnchor", "shortAnchor",
        "active", "externalPosition"};
    const auto object = value.toObject();
    for (auto i = object.begin(); i != object.end(); ++i) {
        if (integers.contains(i.key()) && !(i.key() == "levels" && i.value().isArray()) &&
            !i.value().isDouble())
            throw std::runtime_error("Profile requires JSON integers");
        validateNativeIntegers(i.value());
    }
}

void sqlCheck(bool ok, const QSqlQuery& q)
{
    if (!ok)
        throw std::runtime_error(q.lastError().text().toStdString());
}
}

QJsonObject encodeEngine(const Engine& e, bool records)
{
    QJsonArray cycles, orders, levels, steps;
    if (records)
        for (const auto& c : e.cycles())
            cycles.append(QJsonObject{{"id", s(c.id)}, {"generation", s(c.generation)},
                {"side", static_cast<int>(c.side)}, {"entry", s(c.entry)}, {"take", s(c.take)},
                {"capacity", s(c.capacity)}, {"held", s(c.held)}, {"retired", c.retired}});
    if (records)
        for (const auto& o : e.orders())
            orders.append(QJsonObject{{"label", QString::fromStdString(o.label)},
                {"exchangeId", QString::fromStdString(o.exchangeId)}, {"cycle", s(o.cycle)},
                {"role", static_cast<int>(o.role)}, {"price", s(o.price)}, {"total", s(o.total)},
                {"filled", s(o.filled)}, {"exchangeTime", s(o.exchangeTime)},
                {"status", static_cast<int>(o.status)}, {"mutation", static_cast<int>(o.mutation)},
                {"desiredTotal", s(o.desiredTotal)}});
    for (const auto& l : e.lattice)
        levels.append(QJsonObject{
            {"entry", s(l.entry)}, {"take", s(l.take)}, {"side", static_cast<int>(l.side)}});
    for (const auto& [price, tick] : e.spec.tickSteps)
        steps.append(QJsonObject{{"price", s(price)}, {"tick", s(tick)}});
    QJsonObject root{{"version", 5}, {"config", encodeConfig(e.config)},
        {"profileId", QString::fromStdString(e.profileId)}, {"cycles", cycles}, {"orders", orders},
        {"levels", levels}, {"priceTick", s(e.spec.priceTick)},
        {"quantityStep", s(e.spec.quantityStep)}, {"tickSteps", steps},
        {"generation", s(e.generation)}, {"nextCycle", s(e.nextCycle)},
        {"nextOrder", s(e.nextOrder)}, {"historyFloor", s(e.historyFloor)},
        {"active", static_cast<int>(e.active)}, {"running", e.running},
        {"rebuilding", e.rebuilding}, {"cancelEverything", e.cancelEverything},
        {"blocked", e.blocked}, {"problem", QString::fromStdString(e.problem)}};
    if (e.pendingConfig)
        root["pendingConfig"] = encodeConfig(*e.pendingConfig);
    root["rebuildLongFloor"] = s(e.rebuildFloor[0]);
    root["rebuildShortFloor"] = s(e.rebuildFloor[1]);
    root["rebuildSides"] = static_cast<int>(e.rebuildSides);
    root["longAnchor"] = s(e.anchors[0]);
    root["shortAnchor"] = s(e.anchors[1]);
    root["positionBlocked"] = e.positionBlocked;
    if (e.externalPosition)
        root["externalPosition"] = s(*e.externalPosition);
    return root;
}

Engine decodeEngine(const QJsonObject& o)
{
    const int version = bounded(o, "version", 1, 5);
    if (version >= 4)
        validateNativeIntegers(o);
    if (!o["problem"].isString())
        throw std::runtime_error("Invalid stored problem text");
    Engine e;
    e.config = decodeConfig(o["config"].toObject());
    e.profileId = o["profileId"].toString().toStdString();
    if (e.profileId.empty() || e.profileId.size() > 20 ||
        e.profileId.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        throw std::runtime_error("Invalid profile ID");
    e.spec.priceTick = n(o, "priceTick");
    e.spec.quantityStep = n(o, "quantityStep");
    e.generation = n(o, "generation");
    e.nextCycle = n(o, "nextCycle");
    e.nextOrder = n(o, "nextOrder");
    if (o.contains("historyFloor"))
        e.historyFloor = n(o, "historyFloor");
    if (version < 5) {
        const Int floor = o.contains("rebuildFloor") ? n(o, "rebuildFloor") : 1;
        e.rebuildFloor = {floor, floor};
    } else {
        e.rebuildFloor = {n(o, "rebuildLongFloor"), n(o, "rebuildShortFloor")};
        e.rebuildSides = static_cast<unsigned>(bounded(o, "rebuildSides", 0, 3));
        e.anchors = {n(o, "longAnchor"), n(o, "shortAnchor")};
    }
    e.active = static_cast<Side>(bounded(o, "active", 0, 2));
    e.running = boolean(o, "running");
    e.rebuilding = boolean(o, "rebuilding");
    e.cancelEverything = boolean(o, "cancelEverything");
    e.blocked = boolean(o, "blocked");
    e.problem = o["problem"].toString().toStdString();
    if (o.contains("pendingConfig"))
        e.pendingConfig = decodeConfig(o["pendingConfig"].toObject());
    if (version < 5 && e.rebuilding)
        e.rebuildSides = 3;
    if ((e.rebuilding != (e.rebuildSides != 0)) || (e.pendingConfig && !e.rebuilding))
        throw std::runtime_error("Invalid pending rebuild selection");
    // After a process restart, recalculate an interrupted auto move for its side.
    // A live reconnect retains the in-memory move and unchanged rounded levels.
    if (e.rebuilding && !e.pendingConfig)
        e.pendingConfig = e.config;
    e.positionBlocked = o.contains("positionBlocked") ? boolean(o, "positionBlocked") : false;
    if (o.contains("externalPosition"))
        e.externalPosition = n(o, "externalPosition");
    const bool validRebuildFloors =
        std::none_of(e.rebuildFloor.begin(), e.rebuildFloor.end(), [&](Int floor) {
            return floor <= 0 || floor > e.nextCycle;
        });
    const bool validAnchors =
        validRebuildFloors && std::none_of(e.anchors.begin(), e.anchors.end(), [](Int anchor) {
            return anchor < 0 || anchor > 10000000LL * Scale;
        });
    const bool validCounters =
        e.historyFloor > 0 && e.historyFloor <= e.nextCycle && e.nextCycle > 0 && e.nextOrder > 0;
    const bool validSpec = e.generation >= 0 && e.spec.priceTick >= 0 && e.spec.quantityStep >= 0;
    if (!validRebuildFloors || !validAnchors || !validCounters || !validSpec)
        throw std::runtime_error("Invalid stored counters/specification");
    for (const auto* key : {"cycles", "orders", "levels", "tickSteps"})
        if (!o[key].isArray())
            throw std::runtime_error("Invalid stored record array");
    if (e.spec.priceTick > 1000000000LL * Scale || e.spec.quantityStep > 1000000000LL * Scale)
        throw std::runtime_error("Stored specification overflow");
    for (const auto& v : o["cycles"].toArray()) {
        auto j = v.toObject();
        Cycle c{n(j, "id"), n(j, "generation"), static_cast<Side>(bounded(j, "side", 0, 1)),
            n(j, "entry"), n(j, "take"), n(j, "capacity"), n(j, "held"), boolean(j, "retired")};
        if (c.id >= e.nextCycle)
            throw std::runtime_error("Cycle ID exceeds stored counter");
        e.importCycle(c);
    }
    for (const auto& v : o["orders"].toArray()) {
        auto j = v.toObject();
        Order r;
        if (!j["exchangeId"].isString() || !j["label"].isString())
            throw std::runtime_error("Invalid stored order identifiers");
        r.label = j["label"].toString().toStdString();
        r.exchangeId = j["exchangeId"].toString().toStdString();
        r.cycle = n(j, "cycle");
        r.role = static_cast<Role>(bounded(j, "role", 0, 1));
        r.price = n(j, "price");
        r.total = n(j, "total");
        r.filled = n(j, "filled");
        r.exchangeTime = n(j, "exchangeTime");
        r.status = static_cast<Status>(bounded(j, "status", 0, 4));
        r.mutation = static_cast<Mutation>(bounded(j, "mutation", 0, 3));
        r.desiredTotal = n(j, "desiredTotal");
        const auto prefix = "dg_" + e.profileId + "_" + std::to_string(r.cycle) +
                            (r.role == Role::Entry ? "_e_" : "_t_");
        bool labelNumberOk = false;
        const auto labelNumber =
            QString::fromStdString(r.label.substr(std::min(prefix.size(), r.label.size())))
                .toLongLong(&labelNumberOk);
        if (!r.label.starts_with(prefix) || !labelNumberOk || labelNumber <= 0 ||
            labelNumber >= e.nextOrder)
            throw std::runtime_error("Stored order label does not belong to this profile/role");
        e.importOrder(r);
    }
    for (const auto& v : o["levels"].toArray()) {
        const auto j = v.toObject();
        const Side side = version == 1 ? e.active : static_cast<Side>(bounded(j, "side", 0, 1));
        if (side != Side::Long && side != Side::Short)
            throw std::runtime_error("Invalid grid level side");
        const Level level{n(j, "entry"), n(j, "take"), side};
        const auto& band = side == Side::Long ? e.config.longs : e.config.shorts;
        if (level.entry < band.low || level.entry > band.high || level.take <= 0 ||
            (side == Side::Long ? level.entry >= level.take : level.entry <= level.take))
            throw std::runtime_error("Invalid stored grid level");
        e.lattice.push_back(level);
    }
    for (const auto& v : o["tickSteps"].toArray()) {
        const auto j = v.toObject();
        if (n(j, "price") < 0 || n(j, "price") > 1000000000LL * Scale || n(j, "tick") <= 0 ||
            n(j, "tick") > 1000000000LL * Scale)
            throw std::runtime_error("Invalid tick step");
        e.spec.tickSteps.emplace_back(n(j, "price"), n(j, "tick"));
    }
    if (!e.cycles().empty() && (e.spec.quantityStep <= 0 || e.spec.priceTick <= 0))
        throw std::runtime_error("Missing instrument specification");
    return e;
}

Store::Store(const QString& path)
{
    connection = QUuid::createUuid().toString();
    db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    try {
        if (!db.open())
            throw std::runtime_error(db.lastError().text().toStdString());
        QSqlQuery q(db);
        sqlCheck(q.exec("PRAGMA journal_mode=WAL"), q);
        sqlCheck(q.exec("PRAGMA synchronous=FULL"), q);
        sqlCheck(q.exec("CREATE TABLE IF NOT EXISTS state (id INTEGER PRIMARY KEY CHECK(id=1), "
                        "payload BLOB NOT NULL)"),
            q);
        sqlCheck(q.exec("CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), "
                        "account TEXT NOT NULL)"),
            q);
        sqlCheck(q.exec("CREATE TABLE IF NOT EXISTS archive (label TEXT PRIMARY KEY, exchange_id "
                        "TEXT, payload BLOB NOT NULL)"),
            q);
        sqlCheck(q.exec("CREATE INDEX IF NOT EXISTS archive_exchange ON archive(exchange_id)"), q);
        sqlCheck(q.exec("PRAGMA temp_store=FILE"), q);
        sqlCheck(q.exec("CREATE TEMP TABLE fee_trades (key TEXT PRIMARY KEY)"), q);
        sqlCheck(q.exec("CREATE TEMP TABLE fees (id TEXT PRIMARY KEY, expected INTEGER NOT NULL "
                        "DEFAULT 0, covered INTEGER NOT NULL DEFAULT 0)"),
            q);
        sqlCheck(q.exec("CREATE INDEX fee_missing ON fees(id) WHERE expected > covered"), q);
    } catch (...) {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(connection);
        throw;
    }
}

Store::~Store()
{
    db.close();
    db = {};
    QSqlDatabase::removeDatabase(connection);
}

void Store::resetProfile(const Engine& fresh)
{
    if (!db.transaction())
        throw std::runtime_error("Cannot begin profile reset");
    try {
        QSqlQuery q(db);
        sqlCheck(q.exec("DELETE FROM archive"), q);
        sqlCheck(q.exec("DELETE FROM state"), q);
        sqlCheck(q.exec("DELETE FROM fees"), q);
        sqlCheck(q.exec("DELETE FROM fee_trades"), q);
        sqlCheck(q.prepare("INSERT INTO state (id, payload) VALUES (1, ?)"), q);
        q.addBindValue(QJsonDocument(encodeEngine(fresh)).toJson(QJsonDocument::Compact));
        sqlCheck(q.exec(), q);
        if (!db.commit())
            throw std::runtime_error("Cannot commit profile reset");
        lastEngine = nullptr;
        lastSaved.clear();
        lastMetadata = {};
        lastRevision = ~std::uint64_t(0);
    } catch (...) {
        db.rollback();
        throw;
    }
}

bool Store::load(Engine& e)
{
    QSqlQuery q(db);
    sqlCheck(q.exec("SELECT payload FROM state WHERE id=1"), q);
    if (!q.next())
        return false;
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(q.value(0).toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Corrupt state JSON");
    e = decodeEngine(doc.object());
    lastSaved = q.value(0).toByteArray();
    lastEngine = nullptr;
    lastMetadata = {};
    lastRevision = ~std::uint64_t(0);
    return true;
}

void Store::save(Engine& e)
{
    const auto metadata = encodeEngine(e, false);
    if (lastEngine == &e && lastRevision == e.revision() && metadata == lastMetadata)
        return;
    auto snapshot = encodeEngine(e);
    std::vector<Cycle> keptCycles;
    std::vector<Order> keptOrders;
    QJsonArray keptJson, archived;
    const auto allOrders = snapshot["orders"].toArray();
    std::vector<size_t> completed;
    for (size_t i = 0; i < e.orders().size(); ++i)
        if (!e.orders()[i].live() && e.orders()[i].mutation == Mutation::None)
            completed.push_back(i);
    std::vector<bool> discard(e.orders().size(), false);
    if (completed.size() > 1000) {
        std::nth_element(
            completed.begin(), completed.begin() + 1000, completed.end(), [&](size_t a, size_t b) {
                const auto ta = e.orders()[a].exchangeTime, tb = e.orders()[b].exchangeTime;
                return ta == tb ? a > b : ta > tb;
            });
        for (size_t i = 1000; i < completed.size(); ++i)
            discard[completed[i]] = true;
    }
    for (size_t i = 0; i < e.orders().size(); ++i) {
        if (discard[i])
            archived.append(allOrders[static_cast<qsizetype>(i)]);
        else {
            keptOrders.push_back(e.orders()[i]);
            keptJson.append(allOrders[static_cast<qsizetype>(i)]);
        }
    }
    std::set<Int> needed;
    for (const auto& o : keptOrders)
        needed.insert(o.cycle);
    QJsonArray cyclesJson;
    const auto allCycles = snapshot["cycles"].toArray();
    for (size_t i = 0; i < e.cycles().size(); ++i) {
        const auto& c = e.cycles()[i];
        if (!c.retired || c.held > 0 || needed.contains(c.id)) {
            keptCycles.push_back(c);
            cyclesJson.append(allCycles[static_cast<qsizetype>(i)]);
        }
    }
    snapshot["orders"] = keptJson;
    snapshot["cycles"] = cyclesJson;
    const auto payload = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (payload == lastSaved) {
        lastMetadata = metadata;
        lastRevision = e.revision();
        lastEngine = &e;
        return;
    }
    if (!db.transaction())
        throw std::runtime_error("Cannot begin state transaction");
    try {
        for (const auto& value : archived) {
            const auto order = value.toObject();
            const auto* c = e.cycle(n(order, "cycle"));
            if (!c)
                throw std::runtime_error("Archive order without cycle");
            Engine record;
            record.config = e.config;
            record.spec = e.spec;
            record.profileId = e.profileId;
            record.nextCycle = e.nextCycle;
            record.nextOrder = e.nextOrder;
            record.generation = e.generation;
            record.importCycle(*c);
            // Encode the archived record using the already validated snapshot representation.
            auto json = encodeEngine(record);
            json["orders"] = QJsonArray{order};
            QSqlQuery archive(db);
            archive.prepare(
                "INSERT INTO archive(label,exchange_id,payload) VALUES(?,?,?) ON CONFLICT(label) "
                "DO UPDATE SET exchange_id=excluded.exchange_id,payload=excluded.payload");
            archive.addBindValue(order["label"].toString());
            archive.addBindValue(order["exchangeId"].toString());
            archive.addBindValue(QJsonDocument(json).toJson(QJsonDocument::Compact));
            sqlCheck(archive.exec(), archive);
        }
        QSqlQuery q(db);
        q.prepare("INSERT INTO state(id,payload) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET "
                  "payload=excluded.payload");
        q.addBindValue(payload);
        sqlCheck(q.exec(), q);
        if (!db.commit())
            throw std::runtime_error("Cannot commit state transaction");
        lastSaved = payload;
        if (!archived.isEmpty() || keptCycles.size() != e.cycles().size())
            e.restoreRecords(std::move(keptCycles), std::move(keptOrders));
        lastMetadata = encodeEngine(e, false);
        lastRevision = e.revision();
        lastEngine = &e;
    } catch (...) {
        db.rollback();
        throw;
    }
}

void Store::bindAccount(const QString& identity)
{
    QSqlQuery q(db);
    sqlCheck(q.exec("SELECT account FROM identity WHERE id=1"), q);
    if (q.next()) {
        if (q.value(0).toString() != identity)
            throw std::runtime_error(
                "This profile belongs to a different account/environment/instrument");
        return;
    }
    q.prepare("INSERT INTO identity(id,account) VALUES(1,?)");
    q.addBindValue(identity);
    sqlCheck(q.exec(), q);
}

bool Store::recoverOrder(const std::string& label, Engine& e)
{
    QSqlQuery q(db);
    q.prepare("SELECT payload FROM archive WHERE label=?");
    q.addBindValue(QString::fromStdString(label));
    sqlCheck(q.exec(), q);
    if (!q.next())
        return false;
    const auto archived = decodeEngine(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
    const auto* o = archived.order(label);
    if (!o)
        throw std::runtime_error("Corrupt archived order");
    if (o->cycle < e.historyFloor)
        return false;
    if (!e.cycle(o->cycle)) {
        auto c = *archived.cycle(o->cycle);
        c.retired = true;
        c.held = 0;
        e.importCycle(c);
    }
    e.importOrder(*o);
    return true;
}

bool Store::ownsExchangeId(const QString& id)
{
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM archive WHERE exchange_id=? UNION ALL SELECT 1 FROM fees WHERE id=? "
              "LIMIT 1");
    q.addBindValue(id);
    q.addBindValue(id);
    sqlCheck(q.exec(), q);
    return q.next();
}

void Store::expectFee(const QString& id, Int lots)
{
    QSqlQuery q(db);
    q.prepare("INSERT INTO fees(id,expected) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET "
              "expected=expected+excluded.expected");
    const Int existing = feeValue(id);
    if (lots < 0 || existing > std::numeric_limits<Int>::max() - lots)
        throw std::runtime_error("Fee quantity overflow");
    q.addBindValue(id);
    q.addBindValue(qlonglong(lots));
    sqlCheck(q.exec(), q);
}

Int Store::feeValue(const QString& id, bool covered)
{
    QSqlQuery q(db);
    q.prepare(
        covered ? "SELECT covered FROM fees WHERE id=?" : "SELECT expected FROM fees WHERE id=?");
    q.addBindValue(id);
    sqlCheck(q.exec(), q);
    return q.next() ? q.value(0).toLongLong() : 0;
}

bool Store::recordFee(const QString& key, const QString& id, Int lots)
{
    if (lots < 0 || feeValue(id, true) > std::numeric_limits<Int>::max() - lots)
        throw std::runtime_error("Fee quantity overflow");
    if (!db.transaction())
        throw std::runtime_error("Cannot begin fee transaction");
    try {
        QSqlQuery q(db);
        q.prepare("INSERT OR IGNORE INTO fee_trades(key) VALUES(?)");
        q.addBindValue(key);
        sqlCheck(q.exec(), q);
        const bool fresh = q.numRowsAffected() != 0;
        if (fresh) {
            q.prepare("INSERT INTO fees(id,covered) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET "
                      "covered=covered+excluded.covered");
            q.addBindValue(id);
            q.addBindValue(qlonglong(lots));
            sqlCheck(q.exec(), q);
        }
        if (!db.commit())
            throw std::runtime_error("Cannot commit fee transaction");
        return fresh;
    } catch (...) {
        db.rollback();
        throw;
    }
}

QStringList Store::missingFees()
{
    QSqlQuery q(db);
    sqlCheck(q.exec("SELECT id FROM fees WHERE expected > covered"), q);
    QStringList ids;
    while (q.next())
        ids.append(q.value(0).toString());
    return ids;
}

void Store::resetFees()
{
    // Keep trade IDs so a duplicate received at the reset boundary cannot add an old fee again.
    QSqlQuery q(db);
    sqlCheck(q.exec("DELETE FROM fees"), q);
}

bool Store::feesComplete()
{
    QSqlQuery q(db);
    sqlCheck(q.exec("SELECT 1 FROM fees WHERE expected > covered LIMIT 1"), q);
    return !q.next();
}
