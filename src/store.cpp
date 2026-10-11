#include "store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>
#include <set>

using namespace grid;
namespace {
QString s(Int value) { return QString::number(value); }
Int n(const QJsonObject& o, const char* key) {
    bool ok = false; const Int v = o[key].toString().toLongLong(&ok);
    if (!ok) throw std::runtime_error(std::string("Invalid stored integer: ") + key);
    return v;
}
QJsonObject encodeBand(const Band& b) { return {{"low", s(b.low)}, {"high", s(b.high)}, {"budget", s(b.budget)}, {"levels", b.levels}}; }
Band decodeBand(const QJsonObject& o) { return {n(o, "low"), n(o, "high"), n(o, "budget"), o["levels"].toInt()}; }
QJsonObject encodeConfig(const Config& c) {
    return {{"instrument", QString::fromStdString(c.instrument)}, {"production", c.production}, {"long", encodeBand(c.longs)}, {"short", encodeBand(c.shorts)}};
}
Config decodeConfig(const QJsonObject& o) {
    Config c; c.instrument = o["instrument"].toString().toStdString(); c.production = o["production"].toBool();
    c.longs = decodeBand(o["long"].toObject()); c.shorts = decodeBand(o["short"].toObject());
    const auto error = Engine::validate(c); if (!error.empty()) throw std::runtime_error(error);
    return c;
}
void sqlCheck(bool ok, const QSqlQuery& q) { if (!ok) throw std::runtime_error(q.lastError().text().toStdString()); }
}
QJsonObject encodeEngine(const Engine& e) {
    QJsonArray cycles, orders, levels, steps;
    for (const auto& c : e.cycles) cycles.append(QJsonObject{{"id", s(c.id)}, {"generation", s(c.generation)}, {"side", static_cast<int>(c.side)},
        {"entry", s(c.entry)}, {"take", s(c.take)}, {"capacity", s(c.capacity)}, {"held", s(c.held)}, {"retired", c.retired}});
    for (const auto& o : e.orders) orders.append(QJsonObject{{"label", QString::fromStdString(o.label)}, {"exchangeId", QString::fromStdString(o.exchangeId)},
        {"cycle", s(o.cycle)}, {"role", static_cast<int>(o.role)}, {"price", s(o.price)}, {"total", s(o.total)}, {"filled", s(o.filled)},
        {"exchangeTime", s(o.exchangeTime)}, {"status", static_cast<int>(o.status)}, {"mutation", static_cast<int>(o.mutation)}, {"desiredTotal", s(o.desiredTotal)}});
    for (const auto& l : e.lattice) levels.append(QJsonObject{{"entry", s(l.entry)}, {"take", s(l.take)}, {"side", static_cast<int>(l.side)}});
    for (const auto& [price, tick] : e.spec.tickSteps) steps.append(QJsonObject{{"price", s(price)}, {"tick", s(tick)}});
    QJsonObject root{{"version", 3}, {"config", encodeConfig(e.config)}, {"profileId", QString::fromStdString(e.profileId)},
        {"cycles", cycles}, {"orders", orders}, {"levels", levels}, {"priceTick", s(e.spec.priceTick)}, {"quantityStep", s(e.spec.quantityStep)}, {"tickSteps", steps},
        {"generation", s(e.generation)}, {"nextCycle", s(e.nextCycle)}, {"nextOrder", s(e.nextOrder)}, {"active", static_cast<int>(e.active)},
        {"running", e.running}, {"rebuilding", e.rebuilding}, {"cancelEverything", e.cancelEverything}, {"blocked", e.blocked}, {"problem", QString::fromStdString(e.problem)}};
    if (e.pendingConfig) root["pendingConfig"] = encodeConfig(*e.pendingConfig);
    root["positionBlocked"] = e.positionBlocked;
    if (e.externalPosition) root["externalPosition"] = s(*e.externalPosition);
    return root;
}
Engine decodeEngine(const QJsonObject& o) {
    const int version = o["version"].toInt();
    if (version < 1 || version > 3) throw std::runtime_error("Unsupported state version");
    Engine e; e.config = decodeConfig(o["config"].toObject()); e.profileId = o["profileId"].toString().toStdString();
    if (e.profileId.empty() || e.profileId.size() > 20) throw std::runtime_error("Invalid profile ID");
    e.spec.priceTick = n(o, "priceTick"); e.spec.quantityStep = n(o, "quantityStep");
    e.generation = n(o, "generation"); e.nextCycle = n(o, "nextCycle"); e.nextOrder = n(o, "nextOrder");
    e.active = static_cast<Side>(o["active"].toInt()); e.running = o["running"].toBool(); e.rebuilding = o["rebuilding"].toBool();
    e.cancelEverything = o["cancelEverything"].toBool(); e.blocked = o["blocked"].toBool(); e.problem = o["problem"].toString().toStdString();
    if (o.contains("pendingConfig")) e.pendingConfig = decodeConfig(o["pendingConfig"].toObject());
    e.positionBlocked = o["positionBlocked"].toBool();
    if (o.contains("externalPosition")) e.externalPosition = n(o, "externalPosition");
    std::set<Int> ids; std::set<std::string> labels;
    for (const auto& v : o["cycles"].toArray()) {
        auto j = v.toObject(); Cycle c{n(j, "id"), n(j, "generation"), static_cast<Side>(j["side"].toInt()), n(j, "entry"), n(j, "take"), n(j, "capacity"), n(j, "held"), j["retired"].toBool()};
        if (c.id <= 0 || c.id >= e.nextCycle || !ids.insert(c.id).second || c.capacity <= 0 || c.held < 0 || c.held > c.capacity || c.entry <= 0 || c.take <= 0 ||
            (c.side != Side::Long && c.side != Side::Short)) throw std::runtime_error("Corrupt cycle state");
        e.cycles.push_back(c);
    }
    for (const auto& v : o["orders"].toArray()) {
        auto j = v.toObject(); Order r;
        r.label = j["label"].toString().toStdString(); r.exchangeId = j["exchangeId"].toString().toStdString(); r.cycle = n(j, "cycle");
        r.role = static_cast<Role>(j["role"].toInt()); r.price = n(j, "price"); r.total = n(j, "total"); r.filled = n(j, "filled");
        r.exchangeTime = n(j, "exchangeTime"); r.status = static_cast<Status>(j["status"].toInt()); r.mutation = static_cast<Mutation>(j["mutation"].toInt()); r.desiredTotal = n(j, "desiredTotal");
        if (!e.cycle(r.cycle) || !labels.insert(r.label).second || r.label.empty() || r.label.size() > 64 || r.total <= 0 || r.filled < 0 || r.filled > r.total ||
            static_cast<int>(r.role) < 0 || static_cast<int>(r.role) > 1 || static_cast<int>(r.status) < 0 || static_cast<int>(r.status) > 4 ||
            static_cast<int>(r.mutation) < 0 || static_cast<int>(r.mutation) > 3) throw std::runtime_error("Corrupt order state");
        e.orders.push_back(r);
    }
    for (const auto& v : o["levels"].toArray()) {
        const auto j = v.toObject(); const Side side = version == 1 ? e.active : static_cast<Side>(j["side"].toInt(-1));
        if (side != Side::Long && side != Side::Short) throw std::runtime_error("Invalid grid level side");
        e.lattice.push_back({n(j, "entry"), n(j, "take"), side});
    }
    for (const auto& v : o["tickSteps"].toArray()) { const auto j = v.toObject(); e.spec.tickSteps.emplace_back(n(j, "price"), n(j, "tick")); }
    if (!e.cycles.empty() && (e.spec.quantityStep <= 0 || e.spec.priceTick <= 0)) throw std::runtime_error("Missing instrument specification");
    return e;
}
Store::Store(const QString& path) {
    connection = QUuid::createUuid().toString(); db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(path);
    if (!db.open()) throw std::runtime_error(db.lastError().text().toStdString());
    QSqlQuery q(db);
    sqlCheck(q.exec("PRAGMA journal_mode=WAL"), q); sqlCheck(q.exec("PRAGMA synchronous=FULL"), q);
    sqlCheck(q.exec("CREATE TABLE IF NOT EXISTS state (id INTEGER PRIMARY KEY CHECK(id=1), payload BLOB NOT NULL)"), q);
    sqlCheck(q.exec("CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), account TEXT NOT NULL)"), q);
}
Store::~Store() { db.close(); db = {}; QSqlDatabase::removeDatabase(connection); }
bool Store::load(Engine& e) {
    QSqlQuery q(db); sqlCheck(q.exec("SELECT payload FROM state WHERE id=1"), q); if (!q.next()) return false;
    QJsonParseError error; auto doc = QJsonDocument::fromJson(q.value(0).toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("Corrupt state JSON");
    e = decodeEngine(doc.object()); lastSaved = q.value(0).toByteArray(); return true;
}
void Store::save(const Engine& e) {
    const auto payload = QJsonDocument(encodeEngine(e)).toJson(QJsonDocument::Compact);
    if (payload == lastSaved) return;
    if (!db.transaction()) throw std::runtime_error("Cannot begin state transaction");
    try {
        QSqlQuery q(db); q.prepare("INSERT INTO state(id,payload) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET payload=excluded.payload");
        q.addBindValue(payload); sqlCheck(q.exec(), q);
        if (!db.commit()) throw std::runtime_error("Cannot commit state transaction");
        lastSaved = payload;
    } catch (...) { db.rollback(); throw; }
}
void Store::bindAccount(const QString& identity) {
    QSqlQuery q(db); sqlCheck(q.exec("SELECT account FROM identity WHERE id=1"), q);
    if (q.next()) { if (q.value(0).toString() != identity) throw std::runtime_error("This profile belongs to a different account/environment/instrument"); return; }
    q.prepare("INSERT INTO identity(id,account) VALUES(1,?)"); q.addBindValue(identity); sqlCheck(q.exec(), q);
}
