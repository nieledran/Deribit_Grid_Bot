#include "grid.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace grid {
Int Spec::tickAt(Int price) const {
    Int tick = priceTick;
    for (const auto& [above, value] : tickSteps) if (price >= above) tick = std::max(tick, value);
    return tick;
}
bool Order::live() const { return status == Status::Open || status == Status::Pending || status == Status::Uncertain; }
Int Order::remaining() const { return live() ? std::max<Int>(0, total - filled) : 0; }
std::string sideName(Side side) { return side == Side::Long ? "Long" : side == Side::Short ? "Short" : "Outside ranges"; }
std::string Engine::validate(const Config& c) {
    if (c.instrument != "BTC_USDC-PERPETUAL" && c.instrument != "ETH_USDC-PERPETUAL") return "Only BTC/ETH USDC perpetuals are supported";
    for (const Band* b : {&c.longs, &c.shorts}) {
        if (b->budget < 0 || b->budget > 1000000000LL * Dollars) return "Invalid budget";
        if (b->budget && (b->low <= 0 || b->high <= b->low || b->high > 10000000LL * Scale || b->levels < 1 || b->levels > 200))
            return "Require positive bounds, upper > lower and 1-200 intervals";
    }
    if (c.longs.budget && c.shorts.budget && c.longs.high > c.shorts.low) return "Long and short ranges overlap";
    return {};
}
const Band& Engine::band(Side side) const { return side == Side::Long ? config.longs : config.shorts; }
Side Engine::sideAt(Int p) const {
    if (config.shorts.budget && p >= config.shorts.low && p <= config.shorts.high) return Side::Short;
    if (config.longs.budget && p >= config.longs.low && p <= config.longs.high) return Side::Long;
    return Side::None;
}
Cycle* Engine::cycle(Int id) {
    auto i = std::find_if(cycles.begin(), cycles.end(), [=](const Cycle& c) { return c.id == id; });
    return i == cycles.end() ? nullptr : &*i;
}
Order* Engine::order(const std::string& label) {
    auto i = std::find_if(orders.begin(), orders.end(), [&](const Order& o) { return o.label == label; });
    return i == orders.end() ? nullptr : &*i;
}
Order* Engine::liveOrder(Int id, Role role) {
    for (auto& o : orders) if (o.cycle == id && o.role == role && o.live()) return &o;
    return nullptr;
}
bool Engine::buys(const Order& o) const {
    for (const auto& c : cycles) if (c.id == o.cycle) return (c.side == Side::Long) == (o.role == Role::Entry);
    throw std::runtime_error("Order has no cycle");
}
Int Engine::nominal(Int lots, Int price) const {
    const long double n = static_cast<long double>(lots) * spec.quantityStep * price * Dollars / Scale / Scale;
    if (!std::isfinite(n) || n > std::numeric_limits<Int>::max()) throw std::runtime_error("Notional overflow");
    return static_cast<Int>(std::ceil(n));
}
Int Engine::lotsFor(Int money, Int price) const {
    if (money <= 0 || price <= 0 || spec.quantityStep <= 0) return 0;
    const long double raw = static_cast<long double>(money) * Scale * Scale / Dollars / spec.quantityStep / price;
    if (raw > 1000000000000LL) throw std::runtime_error("Quantity is too large");
    Int q = static_cast<Int>(std::floor(raw));
    while (q > 0 && nominal(q, price) > money) --q;
    return q;
}
Int Engine::used(Side side) const {
    Int sum = 0;
    for (const auto& c : cycles) if (c.side == side) {
        Int q = c.retired ? c.held : c.capacity;
        if (c.retired) for (const auto& o : orders) if (o.cycle == c.id && o.role == Role::Entry) q += o.remaining();
        sum += nominal(q, c.entry);
    }
    return sum;
}
int Engine::occupiedSlots(Side side) const {
    int n = 0;
    for (const auto& c : cycles) if (c.side == side) {
        bool occupied = !c.retired || c.held > 0;
        for (const auto& o : orders) if (o.cycle == c.id && o.live()) occupied = true;
        if (occupied) ++n;
    }
    return n;
}
void Engine::quote(Int b, Int a) {
    if (b <= 0 || a < b) return;
    bid = b; ask = a; market = b + (a - b) / 2;
    active = sideAt(market);
}
void Engine::requestRebuild(const Config& next) {
    if (positionBlocked) throw std::runtime_error("Position mismatch: reconcile or clear history after cancelling bot orders");
    if (const auto error = validate(next); !error.empty()) throw std::runtime_error(error);
    pendingConfig = next; rebuilding = true; running = false; cancelEverything = false;
    blocked = false; problem.clear();
}
void Engine::pause() { running = false; rebuilding = false; pendingConfig.reset(); }
void Engine::cancelAll() { pause(); cancelEverything = true; }
void Engine::uncertain(const std::string& label) {
    if (auto* o = order(label)) { o->status = Status::Uncertain; }
    blocked = true; problem = "Uncertain request outcome: exchange reconciliation required";
}
void Engine::disconnected() {
    for (auto& o : orders) if (o.live() || o.mutation != Mutation::None) {
        o.status = Status::Uncertain;
    }
}
void Engine::resolved(const std::string& label) {
    if (auto* o = order(label)) o->mutation = Mutation::None;
}
void Engine::rejected(const std::string& label, const std::string& reason) {
    if (auto* o = order(label)) {
        if (o->mutation == Mutation::Create && o->exchangeId.empty()) o->status = Status::Rejected;
        o->mutation = Mutation::None;
    }
    blocked = true; problem = reason;
}
void Engine::apply(const Update& u) {
    auto* o = order(u.label);
    if (!o) return;
    auto* c = cycle(o->cycle);
    if (!c || u.filled < 0 || u.total < u.filled || u.price != o->price || (!o->exchangeId.empty() && o->exchangeId != u.exchangeId))
        throw std::runtime_error("Inconsistent state of a bot order");
    if (u.filled < o->filled) return;
    const Int delta = u.filled - o->filled;
    if (o->role == Role::Entry) {
        if (delta > c->capacity - c->held) throw std::runtime_error("Entry fill exceeds cycle capacity");
        c->held += delta;
    } else {
        if (delta > c->held) throw std::runtime_error("Take fill exceeds the cycle inventory");
        c->held -= delta;
    }
    o->filled = u.filled; o->exchangeId = u.exchangeId;
    // A stale open snapshot must never resurrect a terminal order.
    if (u.time >= o->exchangeTime || o->status == Status::Uncertain) {
        o->total = u.total; o->exchangeTime = std::max(o->exchangeTime, u.time);
        if (o->status != Status::Terminal || u.terminal) o->status = u.terminal ? Status::Terminal : Status::Open;
    }
    if (c->held < 0 || c->held > c->capacity) throw std::runtime_error("Cycle inventory invariant violated");
}
bool Engine::eligible(bool buy, Int price) const { return market > 0 && (buy ? price < market : price > market); }
bool Engine::crossing(bool buy, Int price, bool foreignOnly) const {
    auto crosses = [&](bool otherBuy, Int otherPrice) { return otherBuy != buy && (buy ? price >= otherPrice : price <= otherPrice); };
    for (const auto& f : foreign) if (f.buy != buy && (f.price <= 0 || crosses(f.buy, f.price))) return true;
    if (!foreignOnly) for (const auto& o : orders) if (o.live() && o.remaining() && crosses(buys(o), o.price)) return true;
    return false;
}
bool Engine::entryConflict(bool buy, Int price) const {
    if (crossing(buy, price)) return true;
    // Same-direction entries can consume the position reserved by a manual reduce-only take.
    return std::any_of(foreign.begin(), foreign.end(), [buy](const ForeignOrder& f) { return f.reduceOnly && f.buy == buy; });
}
Action Engine::mutate(Order& o, Mutation kind, Int total) {
    o.mutation = kind; o.desiredTotal = total;
    return {kind, o.label, o.exchangeId, buys(o), o.role, o.price, total};
}
std::optional<Action> Engine::create(Cycle& c, Role role, Int lots) {
    if (lots <= 0) return {};
    Order o;
    o.label = "dg_" + profileId + "_" + std::to_string(c.id) + (role == Role::Entry ? "_e_" : "_t_") + std::to_string(nextOrder++);
    o.cycle = c.id; o.role = role; o.price = role == Role::Entry ? c.entry : c.take;
    o.total = lots; o.desiredTotal = lots; o.mutation = Mutation::Create;
    orders.push_back(o);
    return Action{Mutation::Create, o.label, {}, (c.side == Side::Long) == (role == Role::Entry), role, o.price, lots};
}
bool Engine::build() {
    if (market <= 0 || spec.priceTick <= 0 || spec.quantityStep <= 0) return false;
    for (auto& c : cycles) c.retired = true;
    if (pendingConfig) config = *pendingConfig;
    pendingConfig.reset(); active = sideAt(market); lattice.clear(); ++generation;
    for (const Side side : {Side::Short, Side::Long}) {
        const auto& b = band(side);
        if (!b.budget) continue;
        const Int end = side == Side::Long ? b.low : b.high;
        // Use the live price inside the band; otherwise build the full band from its near edge.
        const Int anchor = market > b.low && market < b.high ? market : side == Side::Long ? b.high : b.low;
        const Int distance = std::abs(end - anchor);
        Int previous = anchor;
        if (distance > 0) for (int i = 1; i <= b.levels; ++i) {
            Int p = anchor + (side == Side::Long ? -1 : 1) * (distance * i / b.levels);
            Int tick = spec.tickAt(p);
            if (tick <= 0) throw std::runtime_error("Invalid price tick");
            p = side == Side::Long ? (p / tick) * tick : ((p + tick - 1) / tick) * tick;
            const Int takeTick = spec.tickAt(previous);
            Int take = side == Side::Long ? ((previous + takeTick - 1) / takeTick) * takeTick : (previous / takeTick) * takeTick;
            if (p < b.low || p > b.high || (side == Side::Long ? p >= take : p <= take)) {
                blocked = true; problem = sideName(side) + " grid is too dense for the price tick; reduce N"; lattice.clear();
                rebuilding = false; running = true; return true;
            }
            lattice.push_back({p, take, side}); previous = p;
        }
    }
    rebuilding = false; running = true;
    return true;
}
bool Engine::settled() const {
    for (const auto& o : orders) if (o.mutation != Mutation::None || o.status == Status::Uncertain || (o.role == Role::Entry && o.live())) return false;
    if (!cancelEverything) for (const auto& c : cycles) if (c.held > 0) {
        Int protectedLots = 0;
        for (const auto& o : orders) if (o.cycle == c.id && o.role == Role::Take) protectedLots += o.remaining();
        if (protectedLots != c.held) return false;
    }
    if (cancelEverything) for (const auto& o : orders) if (o.live()) return false;
    return true;
}
Int Engine::trackedPosition() const {
    Int lots = 0;
    for (const auto& c : cycles) lots += c.side == Side::Long ? c.held : -c.held;
    return lots * spec.quantityStep;
}
bool Engine::positionMatches(Int actual) const {
    const Int tracked = trackedPosition();
    // An old profile with inventory cannot silently adopt a new external offset.
    return actual == tracked + externalPosition.value_or(tracked == 0 ? actual : 0);
}
bool Engine::reconcilePosition(Int actual) {
    if (!positionMatches(actual)) {
        pause(); positionBlocked = true;
        problem = "Account position differs from tracked inventory; entries are paused, existing takes preserved. Review manual trades before resetting";
        return false;
    }
    if (!externalPosition) externalPosition = actual - trackedPosition();
    if (positionBlocked) problem.clear();
    positionBlocked = false; return true;
}
bool Engine::canClearHistory() const {
    return !running && !rebuilding && std::none_of(orders.begin(), orders.end(), [](const Order& o) { return o.live() || o.mutation != Mutation::None; });
}
void Engine::clearHistory(Int actual) {
    if (!canClearHistory()) throw std::runtime_error("Pause and cancel all bot orders before clearing history");
    cycles.clear(); orders.clear(); lattice.clear(); externalPosition = actual;
    positionBlocked = false; blocked = false; problem.clear(); cancelEverything = false;
    // Keep profile identity and monotonic label IDs so old exchange orders cannot be reused.
}
Engine Engine::preview(const Config& next) const {
    if (const auto error = validate(next); !error.empty()) throw std::runtime_error(error);
    Engine view = *this;
    for (auto& o : view.orders) if (o.role == Role::Entry) { o.status = Status::Terminal; o.mutation = Mutation::None; }
    view.pendingConfig = next; view.blocked = false;
    if (!view.build()) throw std::runtime_error("Connect and wait for instrument specifications and a fresh quote");
    if (view.blocked) throw std::runtime_error(view.problem);
    return view;
}
std::optional<Action> Engine::nextAction() {
    problem = blocked || positionBlocked ? problem : std::string{};
    for (const auto& o : orders) if (o.mutation != Mutation::None || o.status == Status::Uncertain) return {};
    for (auto& o : orders) if (o.live() && (cancelEverything || ((!running || rebuilding || blocked || positionBlocked) && o.role == Role::Entry)))
        return mutate(o, Mutation::Cancel);
    if (cancelEverything || positionBlocked) return {};
    // Protect fills before permitting another opening order, including while paused.
    for (auto& c : cycles) if (c.held > 0) {
        const bool buy = c.side == Side::Short;
        auto* take = liveOrder(c.id, Role::Take);
        const Int remaining = take ? take->remaining() : 0;
        if (remaining == c.held) continue;
        if (crossing(buy, c.take)) {
            for (auto& o : orders) if (o.live() && o.role == Role::Entry && buys(o) != buy && (buy ? c.take >= o.price : c.take <= o.price))
                return mutate(o, Mutation::Cancel);
            problem = "Take is blocked by an opposite take or a manual order";
            return {};
        }
        if (take) return mutate(*take, Mutation::Edit, take->filled + c.held);
        return create(c, Role::Take, c.held);
    }
    if (blocked) return {};
    if (rebuilding) { if (!settled() || !build()) return {}; }
    if (!running) return {};
    // A newly added manual order may conflict with an already resting bot entry.
    for (auto& o : orders) if (o.live() && o.role == Role::Entry && entryConflict(buys(o), o.price)) {
        problem = "Entry waits: conflict with a manual order or take";
        return mutate(o, Mutation::Cancel);
    }
    // Existing slots retain their fixed size and prices within this generation.
    for (auto& c : cycles) if (!c.retired) {
        auto* entry = liveOrder(c.id, Role::Entry);
        const Int free = c.capacity - c.held;
        const bool buy = c.side == Side::Long;
        if (free > 0 && eligible(buy, c.entry) && !entryConflict(buy, c.entry)) {
            if (entry) {
                if (entry->remaining() < free) return mutate(*entry, Mutation::Edit, entry->filled + free);
            } else return create(c, Role::Entry, free);
        }
    }
    // Alternate opening sides so both grids start filling their slots immediately.
    const Side first = occupiedSlots(Side::Short) <= occupiedSlots(Side::Long) ? Side::Short : Side::Long;
    const Side second = first == Side::Short ? Side::Long : Side::Short;
    for (const Side side : {first, second}) {
        const auto& b = band(side);
        const int freeSlots = b.levels - occupiedSlots(side);
        const Int freeMoney = b.budget - used(side);
        if (!b.budget || freeSlots <= 0 || freeMoney <= 0) continue;
        for (const auto& l : lattice) {
            if (l.side != side) continue;
            bool occupied = false;
            for (const auto& c : cycles) if (!c.retired && c.side == side && c.entry == l.entry) occupied = true;
            const bool buy = side == Side::Long;
            if (occupied || !eligible(buy, l.entry)) continue;
            if (entryConflict(buy, l.entry)) { problem = "Entry waits: conflict with a manual order or take"; continue; }
            const Int q = lotsFor(freeMoney / freeSlots, l.entry);
            if (!q) { problem = sideName(side) + " available budget is below the minimum quantity"; break; }
            cycles.push_back({nextCycle++, generation, side, l.entry, l.take, q, 0, false});
            return create(cycles.back(), Role::Entry, q);
        }
    }
    return {};
}
}
