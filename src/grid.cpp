#include "grid.h"
#include <algorithm>
#include <cmath>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/version.hpp>
static_assert(BOOST_VERSION == 108900, "Boost 1.89.0 is required");
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
        if (b->levels < 1 || b->levels > 200 || b->low < 0 || b->high < 0 || b->high > 10000000LL * Scale)
            return "Invalid range or interval count";
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
namespace {
using Wide = boost::multiprecision::checked_uint256_t;
constexpr Int Divisor = Scale * (Scale / Dollars);
Int narrow(const Wide& value) {
    if (value > Wide(std::numeric_limits<Int>::max())) throw std::runtime_error("Integer overflow");
    return value.convert_to<Int>();
}
Int add(Int a, Int b) {
    if ((b > 0 && a > std::numeric_limits<Int>::max() - b) ||
        (b < 0 && a < std::numeric_limits<Int>::min() - b)) throw std::runtime_error("Integer overflow");
    return a + b;
}
size_t sideIndex(Side s) {
    if (s != Side::Long && s != Side::Short) throw std::runtime_error("Invalid side");
    return static_cast<size_t>(s);
}
}
const Cycle* Engine::cycle(Int id) const {
    const auto i = cycleIndex_.find(id);
    return i == cycleIndex_.end() ? nullptr : &cycles_[i->second];
}
const Order* Engine::order(const std::string& label) const {
    const auto i = labelIndex_.find(label);
    return i == labelIndex_.end() ? nullptr : &orders_[i->second];
}
const Order* Engine::orderByExchangeId(const std::string& id) const {
    const auto i = exchangeIndex_.find(id);
    return i == exchangeIndex_.end() ? nullptr : &orders_[i->second];
}
Cycle* Engine::mutableCycle(Int id) { return const_cast<Cycle*>(cycle(id)); }
Order* Engine::mutableOrder(const std::string& label) { return const_cast<Order*>(order(label)); }
bool Engine::hasLiveOrders(Int id) const {
    const auto i = links_.find(id);
    return i != links_.end() && (i->second.entry || i->second.take);
}
void Engine::refreshCycle(Int id) {
    const auto* c = cycle(id);
    if (!c) throw std::runtime_error("Order has no cycle");
    auto& link = links_.at(id);
    auto& total = totals_[sideIndex(c->side)];
    Int lots = c->retired ? c->held : c->capacity;
    if (c->retired && link.entry) lots = add(lots, orders_[*link.entry].remaining());
    const Totals next{nominal(lots, c->entry), !c->retired || c->held > 0 || link.entry || link.take ? 1 : 0, c->held > 0 ? 1 : 0};
    total.reserved = add(total.reserved - link.contribution.reserved, next.reserved);
    total.occupied += next.occupied - link.contribution.occupied;
    total.positioned += next.positioned - link.contribution.positioned;
    link.contribution = next;
}
void Engine::refreshOrder(size_t index) {
    auto& o = orders_[index]; auto& link = links_.at(o.cycle);
    auto& slot = o.role == Role::Entry ? link.entry : link.take;
    if (o.live()) {
        if (slot && *slot != index) throw std::runtime_error("Multiple live orders for a cycle role");
        slot = index; liveIndices_.insert(index);
    } else {
        if (slot == index) slot.reset();
        liveIndices_.erase(index);
    }
    if (!o.exchangeId.empty()) {
        auto [it, inserted] = exchangeIndex_.emplace(o.exchangeId, index);
        if (!inserted && it->second != index) throw std::runtime_error("Duplicate exchange order ID");
    }
    refreshCycle(o.cycle); ++revision_;
}
void Engine::importCycle(Cycle c) {
    if (c.id <= 0 || c.generation < 0 || c.capacity <= 0 || c.held < 0 || c.held > c.capacity || c.entry <= 0 || c.take <= 0 || c.entry > 10000000LL * Scale || c.take > 10000000LL * Scale ||
        (c.side != Side::Long && c.side != Side::Short) || cycle(c.id)) throw std::runtime_error("Corrupt cycle state");
    const Int required = nominal(c.retired ? c.held : c.capacity, c.entry);
    (void)add(totals_[sideIndex(c.side)].reserved, required);
    cycleIndex_.emplace(c.id, cycles_.size()); links_.emplace(c.id, Links{}); cycles_.push_back(c);
    refreshCycle(c.id); ++revision_;
}
void Engine::importOrder(Order o) {
    const auto* c = cycle(o.cycle);
    if (!c || o.label.empty() || o.label.size() > 64 || order(o.label) || o.total <= 0 || o.filled < 0 || o.filled > o.total ||
        (o.role != Role::Entry && o.role != Role::Take) || o.price != (o.role == Role::Entry ? c->entry : c->take) ||
        o.exchangeTime < 0 || o.desiredTotal < 0 || static_cast<unsigned>(o.status) > 4 || static_cast<unsigned>(o.mutation) > 3)
        throw std::runtime_error("Corrupt order state");
    const auto& link = links_.at(o.cycle);
    const auto& slot = o.role == Role::Entry ? link.entry : link.take;
    if ((o.live() && slot) || (!o.exchangeId.empty() && exchangeIndex_.contains(o.exchangeId)) ||
        (o.live() && o.remaining() > (o.role == Role::Entry ? c->capacity - c->held : c->held)))
        throw std::runtime_error("Invalid live order linkage/quantity");
    if (o.live() && o.role == Role::Entry && c->retired) {
        const Int required = nominal(add(c->held, o.remaining()), c->entry);
        (void)add(totals_[sideIndex(c->side)].reserved - link.contribution.reserved, required);
    }
    const size_t index = orders_.size(); labelIndex_.emplace(o.label, index); orders_.push_back(std::move(o)); refreshOrder(index);
}
void Engine::restoreRecords(std::vector<Cycle> cycles, std::vector<Order> orders) {
    Engine checked; checked.spec = spec;
    for (auto& c : cycles) checked.importCycle(std::move(c));
    for (auto& o : orders) checked.importOrder(std::move(o));
    cycles_ = std::move(checked.cycles_); orders_ = std::move(checked.orders_);
    cycleIndex_ = std::move(checked.cycleIndex_); labelIndex_ = std::move(checked.labelIndex_);
    exchangeIndex_ = std::move(checked.exchangeIndex_); liveIndices_ = std::move(checked.liveIndices_);
    links_ = std::move(checked.links_); totals_ = checked.totals_; ++revision_;
}
Order* Engine::liveOrder(Int id, Role role) {
    const auto i = links_.find(id); if (i == links_.end()) return nullptr;
    const auto& slot = role == Role::Entry ? i->second.entry : i->second.take;
    return slot ? &orders_[*slot] : nullptr;
}
bool Engine::buys(const Order& o) const {
    const auto* c = cycle(o.cycle); if (!c) throw std::runtime_error("Order has no cycle");
    return (c->side == Side::Long) == (o.role == Role::Entry);
}
Int Engine::nominal(Int lots, Int price) const {
    if (lots < 0 || price < 0 || spec.quantityStep < 0) throw std::runtime_error("Negative notional operand");
    const Wide product = Wide(lots) * spec.quantityStep * price;
    return narrow(product / Divisor + (product % Divisor != 0 ? 1 : 0));
}
Int Engine::lotsFor(Int money, Int price) const {
    if (money <= 0 || price <= 0 || spec.quantityStep <= 0) return 0;
    const Wide lots = Wide(money) * Divisor / (Wide(spec.quantityStep) * price);
    if (lots > Wide(1000000000000LL)) throw std::runtime_error("Quantity is too large");
    return narrow(lots);
}
Int Engine::used(Side side) const { return totals_[sideIndex(side)].reserved; }
int Engine::occupiedSlots(Side side) const { return totals_[sideIndex(side)].occupied; }
int Engine::availableSlots(Side side) const { return std::max(0, band(side).levels - totals_[sideIndex(side)].positioned); }
void Engine::pause() { running = false; rebuilding = false; movingSide_ = Side::None; transitionResize_ = false; pendingConfig.reset(); rebuildSides = 0; ++revision_; }
void Engine::cancelAll() { pause(); cancelEverything = true; }
void Engine::uncertain(const std::string& label) {
    if (auto* o = mutableOrder(label)) { o->status = Status::Uncertain; refreshOrder(labelIndex_.at(label)); }
    blocked = true; problem = "Uncertain request outcome: exchange reconciliation required";
}
void Engine::disconnected() {
    // Freeze order mutations until reconciliation, retaining the current run/rebuild intent.
    // The window's readiness gate also prevents trading while the connection is down.
    for (auto& o : orders_) if (o.live() || o.mutation != Mutation::None) {
        o.status = Status::Uncertain; refreshOrder(labelIndex_.at(o.label));
    }
}
void Engine::resolved(const std::string& label) {
    if (auto* o = mutableOrder(label)) { o->mutation = Mutation::None; ++revision_; }
}
void Engine::rejected(const std::string& label, const std::string& reason) {
    if (auto* o = mutableOrder(label)) {
        if (o->mutation == Mutation::Create && o->exchangeId.empty()) o->status = Status::Rejected;
        o->mutation = Mutation::None; refreshOrder(labelIndex_.at(label));
    }
    blocked = true; problem = reason;
}
void Engine::apply(const Update& u) {
    auto* o = mutableOrder(u.label);
    if (!o) return;
    auto* c = mutableCycle(o->cycle);
    if (!c || u.exchangeId.empty() || u.time < 0 || u.total <= 0 || u.filled < 0 || u.total < u.filled || u.price != o->price || (!o->exchangeId.empty() && o->exchangeId != u.exchangeId))
        throw std::runtime_error("Inconsistent state of a bot order");
    if (const auto i = exchangeIndex_.find(u.exchangeId); i != exchangeIndex_.end() && i->second != labelIndex_.at(u.label))
        throw std::runtime_error("Exchange ID belongs to a different order");
    if (u.filled < o->filled) return;
    const Int allowed = o->mutation == Mutation::Edit ? o->desiredTotal : o->total;
    if (u.total > allowed) throw std::runtime_error("Unexpected exchange quantity");
    if (u.filled == o->filled && u.total == o->total && u.time == o->exchangeTime && u.exchangeId == o->exchangeId &&
        o->status == (u.terminal ? Status::Terminal : Status::Open)) return;
    const Int delta = u.filled - o->filled;
    if (o->role == Role::Entry) {
        c->held = add(c->held, delta);
        if (c->held > c->capacity) {
            // An old cancelled entry can fill after its free quantity was restored.
            // Account for the real fill and make the cycle close-only, rather than losing inventory.
            c->capacity = c->held; c->retired = true;
        }
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
    if (c->retired) if (const auto* entry = liveOrder(c->id, Role::Entry))
        c->capacity = std::max(c->capacity, add(c->held, entry->remaining()));
    if (c->held < 0 || c->held > c->capacity) throw std::runtime_error("Cycle inventory invariant violated");
    refreshOrder(labelIndex_.at(u.label));
}
bool Engine::eligible(bool buy, Int price) const { return market > 0 && (buy ? price < market : price > market); }
bool Engine::crossing(bool buy, Int price, bool foreignOnly) const {
    auto crosses = [&](bool otherBuy, Int otherPrice) { return otherBuy != buy && (buy ? price >= otherPrice : price <= otherPrice); };
    for (const auto& f : foreign) if (f.buy != buy && (f.price <= 0 || crosses(f.buy, f.price))) return true;
    if (!foreignOnly) for (const auto index : liveIndices_) { const auto& o = orders_[index]; if (o.remaining() && crosses(buys(o), o.price)) return true; }
    return false;
}
bool Engine::entryConflict(bool buy, Int price) const {
    if (crossing(buy, price)) return true;
    // Same-direction entries can consume the position reserved by a manual reduce-only take.
    return std::any_of(foreign.begin(), foreign.end(), [buy](const ForeignOrder& f) { return f.reduceOnly && f.buy == buy; });
}
Action Engine::mutate(Order& o, Mutation kind, Int total) {
    o.mutation = kind; o.desiredTotal = total; ++revision_;
    return {kind, o.label, o.exchangeId, buys(o), o.role, o.price, total};
}
std::optional<Action> Engine::create(Cycle& c, Role role, Int lots) {
    if (lots <= 0) return {};
    Order o;
    if (nextOrder == std::numeric_limits<Int>::max()) throw std::runtime_error("Order ID overflow");
    o.label.reserve(64); o.label.append("dg_").append(profileId).append("_").append(std::to_string(c.id))
        .append(role == Role::Entry ? "_e_" : "_t_").append(std::to_string(nextOrder++));
    o.cycle = c.id; o.role = role; o.price = role == Role::Entry ? c.entry : c.take;
    o.total = lots; o.desiredTotal = lots; o.mutation = Mutation::Create;
    importOrder(o);
    return Action{Mutation::Create, o.label, {}, (c.side == Side::Long) == (role == Role::Entry), role, o.price, lots};
}
bool Engine::settled(unsigned sides) const {
    for (const auto& o : orders_) if (o.mutation != Mutation::None || o.status == Status::Uncertain || (o.role == Role::Entry && o.live() && (sides & (1u << sideIndex(cycle(o.cycle)->side))))) return false;
    if (!cancelEverything) for (const auto& c : cycles_) if (c.held > 0) {
        Int protectedLots = 0;
        const auto& link = links_.at(c.id);
        if (link.take) protectedLots = orders_[*link.take].remaining();
        if (protectedLots != c.held) return false;
    }
    if (cancelEverything) for (const auto& o : orders_) if (o.live()) return false;
    return true;
}
Int Engine::trackedPosition() const {
    Int lots = 0;
    for (const auto& c : cycles_) lots = add(lots, c.side == Side::Long ? c.held : -c.held);
    const Int value = narrow(Wide(lots < 0 ? -add(lots, 1) : lots) * spec.quantityStep + (lots < 0 ? spec.quantityStep : 0));
    return lots < 0 ? -value : value;
}
bool Engine::positionMatches(Int actual) const {
    const Int tracked = trackedPosition();
    // An old profile with inventory cannot silently adopt a new external offset.
    return actual == add(tracked, externalPosition.value_or(tracked == 0 ? actual : 0));
}
bool Engine::reconcilePosition(Int actual) {
    if (!positionMatches(actual)) {
        pause(); positionBlocked = true;
        problem = "Account position differs from tracked inventory; entries are paused, existing takes preserved. Review manual trades before resetting";
        return false;
    }
    if (!externalPosition) externalPosition = add(actual, -trackedPosition());
    if (positionBlocked) problem.clear();
    positionBlocked = false; return true;
}
bool Engine::canClearHistory() const {
    return !running && !rebuilding && std::none_of(orders_.begin(), orders_.end(), [](const Order& o) { return o.live() || o.mutation != Mutation::None; });
}
void Engine::clearHistory(Int actual) {
    if (!canClearHistory()) throw std::runtime_error("Pause and cancel all bot orders before clearing history");
    restoreRecords({}, {}); lattice.clear(); externalPosition = actual; historyFloor = nextCycle; rebuildFloor = {1, 1}; anchors = {}; rebuildSides = 0;
    positionBlocked = false; blocked = false; problem.clear(); cancelEverything = false;
    // Keep profile identity and monotonic label IDs so old exchange orders cannot be reused.
}
std::optional<Action> Engine::nextAction() {
    problem = blocked || positionBlocked ? problem : std::string{};
    for (const auto& o : orders_) if (o.mutation != Mutation::None || o.status == Status::Uncertain) return {};
    if (cancelEverything || positionBlocked) {
        for (auto& o : orders_) if (o.live() && (cancelEverything || o.role == Role::Entry)) return mutate(o, Mutation::Cancel);
        return {};
    }
    // Protect fills before permitting another opening order, including while paused.
    for (auto& c : cycles_) if (c.held > 0) {
        const bool buy = c.side == Side::Short;
        auto* take = liveOrder(c.id, Role::Take);
        const Int remaining = take ? take->remaining() : 0;
        if (remaining == c.held) continue;
        if (crossing(buy, c.take)) {
            for (auto& o : orders_) if (o.live() && o.role == Role::Entry && buys(o) != buy && (buy ? c.take >= o.price : c.take <= o.price))
                return mutate(o, Mutation::Cancel);
            problem = "Take is blocked by an opposite take or a manual order";
            return {};
        }
        if (take) return mutate(*take, Mutation::Edit, add(take->filled, c.held));
        return create(c, Role::Take, c.held);
    }
    // Only old-cohort slot changes resize the transition. New fills leave the step alone.
    if (!blocked && !pendingConfig && (running || rebuilding) && movingSide_ == Side::None && (rebuildFloor[0] > 1 || rebuildFloor[1] > 1)) {
        for (const auto side : {Side::Long, Side::Short}) if (band(side).budget && rebuildFloor[sideIndex(side)] > 1) {
            const auto count = std::count_if(lattice.begin(), lattice.end(), [side](const Level& l) { return l.side == side; });
            if (count != transitionSlots(side)) { prepareMove(side, true); break; }
        }
    }
    if (transitionResize_) prepareMove(movingSide_, true); // Include fills/quotes that raced cancellation.
    for (auto& o : orders_) if (o.live() && o.role == Role::Entry &&
        (!running || blocked || cycle(o.cycle)->retired || rebuildCancels(*cycle(o.cycle))))
        return mutate(o, Mutation::Cancel);
    if (blocked) return {};
    if (rebuilding) {
        if (movingSide_ == Side::None) { if (!settled(pendingConfig ? rebuildSides : 3) || !build()) return {}; }
        else { if (!build()) return {}; }
    }
    if (!running) return {};
    // A final fill can arrive after the cancellation snapshot and after a new allocation.
    // Retire opening capacity until old inventory fits the same slot and budget caps.
    for (const auto side : {Side::Long, Side::Short}) {
        const auto& limit = band(side);
        for (auto i = cycles_.rbegin(); i != cycles_.rend() && (used(side) > limit.budget || occupiedSlots(side) > limit.levels); ++i) {
            auto& c = *i;
            if (c.side != side || c.retired || c.capacity == c.held) continue;
            c.retired = true; refreshCycle(c.id); ++revision_;
            if (auto* entry = liveOrder(c.id, Role::Entry)) return mutate(*entry, Mutation::Cancel);
        }
    }
    // A newly added manual order may conflict with an already resting bot entry.
    for (auto& o : orders_) if (o.live() && o.role == Role::Entry && entryConflict(buys(o), o.price)) {
        problem = "Entry waits: conflict with a manual order or take";
        return mutate(o, Mutation::Cancel);
    }
    // Existing slots retain their fixed size and prices within this generation.
    for (auto& c : cycles_) if (!c.retired && used(c.side) <= band(c.side).budget && occupiedSlots(c.side) <= band(c.side).levels) {
        auto* entry = liveOrder(c.id, Role::Entry);
        const Int free = c.capacity - c.held;
        const bool buy = c.side == Side::Long;
        if (free > 0 && eligible(buy, c.entry) && !entryConflict(buy, c.entry)) {
            if (entry) {
                if (entry->remaining() < free) return mutate(*entry, Mutation::Edit, add(entry->filled, free));
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
        // Positioned new cycles keep their original repeat level even when the lattice moves.
        // Reserve near levels for those off-lattice allocations so free entries still reach the end.
        int offLattice = 0;
        for (const auto& c : cycles_) if (c.side == side && c.id >= rebuildFloor[sideIndex(side)] &&
            (!c.retired || c.held > 0) && std::none_of(lattice.begin(), lattice.end(), [&](const Level& l) { return l.side == side && l.entry == c.entry; })) ++offLattice;
        for (const auto& l : lattice) {
            if (l.side != side) continue;
            bool occupied = false;
            for (const auto& c : cycles_) if (!c.retired && c.side == side && c.entry == l.entry) occupied = true;
            if (!occupied && offLattice > 0) { --offLattice; continue; }
            const bool buy = side == Side::Long;
            if (occupied || !eligible(buy, l.entry)) continue;
            if (entryConflict(buy, l.entry)) { problem = "Entry waits: conflict with a manual order or take"; continue; }
            const Int q = lotsFor(freeMoney / freeSlots, l.entry);
            if (!q) { problem = sideName(side) + " available budget is below the minimum quantity"; break; }
            if (nextCycle == std::numeric_limits<Int>::max()) throw std::runtime_error("Cycle ID overflow");
            importCycle({nextCycle++, generation, side, l.entry, l.take, q, 0, false});
            return create(cycles_.back(), Role::Entry, q);
        }
    }
    return {};
}
}
