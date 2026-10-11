#include "grid.h"
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace grid;
int checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
Int p(double v) { return static_cast<Int>(std::llround(v * Scale)); }
Engine setup(int n = 10, bool both = false) {
    Engine e; e.profileId = "test"; e.spec = {p(0.1), p(0.0001), {}};
    e.config.longs = {p(80000), p(84500), both ? 850 * Dollars : 0, n};
    e.config.shorts = {p(84500), p(90000), 850 * Dollars, n};
    e.quote(p(84599.9), p(84600.1)); e.requestRebuild(e.config);
    return e;
}
Update snapshot(const Order& o, Int filled, bool terminal = false, Int time = 1) {
    return {o.label, o.exchangeId.empty() ? "ex-" + o.label : o.exchangeId, o.total, filled, time, o.price, terminal};
}
void ack(Engine& e, const Action& a, Int filled = 0, bool terminal = false, Int time = 1) {
    auto o = *e.order(a.label);
    if (a.kind == Mutation::Edit) o.total = a.total;
    auto u = snapshot(o, filled, terminal || a.kind == Mutation::Cancel, time);
    e.apply(u); e.resolved(a.label);
}
Action requireAction(Engine& e, Mutation kind, Role role) {
    auto a = e.nextAction(); check(a.has_value(), "Expected an action");
    if (a->kind != kind || a->role != role) throw std::runtime_error("Unexpected action " + a->label + ": kind=" + std::to_string(static_cast<int>(a->kind)) + ", role=" + std::to_string(static_cast<int>(a->role)) + "; expected kind=" + std::to_string(static_cast<int>(kind)) + ", role=" + std::to_string(static_cast<int>(role)));
    ++checks; return *a;
}
void completePause(Engine& e) {
    for (int i = 0; i < 100; ++i) {
        auto a = e.nextAction(); if (!a) break;
        ack(e, *a, e.order(a->label)->filled, false, 50 + i);
    }
}
void pricesAndCaps() {
    auto e = setup(10, true); auto a = requireAction(e, Mutation::Create, Role::Entry);
    check(!a.buy && a.price == p(85140), "Short grid first price must be 85140");
    check(e.lattice.size() == 20 && e.lattice[9].entry == p(90000) && e.lattice.back().entry == p(80000), "Both lattices must cover their ranges");
    check(e.cycles.front().take == p(84600), "First take must be the anchor");
    ack(e, a);
    auto opposite = requireAction(e, Mutation::Create, Role::Entry);
    check(opposite.buy, "Second placement starts the other grid immediately"); ack(e, opposite);
    for (int i = 0; i < 20; ++i) { auto next = e.nextAction(); if (!next) break; ack(e, *next); }
    check(e.occupiedSlots(Side::Short) == 10, "Must allocate ten slots");
    check(e.occupiedSlots(Side::Long) == 10, "Must also allocate ten long slots");
    check(e.used(Side::Short) <= e.config.shorts.budget, "Budget must not be exceeded");
    check(e.used(Side::Long) <= e.config.longs.budget, "Long budget must not be exceeded");
    check(e.sideAt(p(84500)) == Side::Short, "Boundary belongs to short");
    check(e.sideAt(p(80000)) == Side::Long && e.sideAt(p(90001)) == Side::None, "Range selection");
}
void partialAndDuplicate() {
    auto e = setup(1); auto entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry);
    const auto o = *e.order(entry.label);
    check(o.total >= 4, "Test needs several lots");
    e.apply(snapshot(o, 2, false, 2)); e.apply(snapshot(o, 2, false, 2));
    e.apply(snapshot(o, 1, false, 1));
    check(e.cycles[0].held == 2, "Duplicate and stale fills must be ignored");
    auto take = requireAction(e, Mutation::Create, Role::Take);
    check(take.buy && take.total == 2, "Take must protect exactly the filled lots"); ack(e, take);
    e.apply(snapshot(o, 4, false, 3));
    auto edit = requireAction(e, Mutation::Edit, Role::Take); check(edit.total == 4, "Aggregate another partial fill");
    ack(e, edit, 1, false, 4);
    check(e.cycles[0].held == 3, "Partial take decreases inventory");
    e.pause(); auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel, 5, false, 5);
    check(e.cycles[0].held == 4, "A fill racing cancellation must be counted");
    edit = requireAction(e, Mutation::Edit, Role::Take);
    check(edit.total == 5, "Edit total includes already filled take quantity"); ack(e, edit, 1, false, 6);
    check(e.settled(), "Paused inventory must be protected");
}
void partialTakeReentry() {
    auto e = setup(1); auto entry = requireAction(e, Mutation::Create, Role::Entry);
    const Int q = entry.total; ack(e, entry, q, true);
    auto take = requireAction(e, Mutation::Create, Role::Take); ack(e, take);
    e.apply(snapshot(*e.order(take.label), 2, false, 2));
    auto reentry = requireAction(e, Mutation::Create, Role::Entry);
    check(reentry.total == 2 && reentry.price == entry.price, "Only the closed fragment may reenter");
    check(e.used(Side::Short) <= e.config.shorts.budget, "Reentry reserves the same budget");
    e = setup(1); entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, 4, false);
    take = requireAction(e, Mutation::Create, Role::Take); ack(e, take, 2, false, 2);
    auto increase = requireAction(e, Mutation::Edit, Role::Entry);
    check(increase.total == entry.total + 2, "A closed fragment increases the surviving entry, without duplication");
}
void rebuildAndOldSlots() {
    auto e = setup(2); auto entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, entry.total, true);
    auto take = requireAction(e, Mutation::Create, Role::Take); ack(e, take);
    auto next = e.config; next.shorts.levels = 3;
    e.requestRebuild(next);
    auto fresh = requireAction(e, Mutation::Create, Role::Entry); ack(e, fresh);
    check(e.cycles[0].retired && e.cycles[0].take == take.price, "Old take must stay fixed");
    check(e.lattice.size() == 3 && e.occupiedSlots(Side::Short) == 2, "Old cycle consumes one of new slots");
    fresh = requireAction(e, Mutation::Create, Role::Entry); ack(e, fresh);
    check(!e.nextAction() && e.occupiedSlots(Side::Short) == 3, "No extra slots while old take is open");
    e.apply(snapshot(*e.order(take.label), take.total, true, 10));
    fresh = requireAction(e, Mutation::Create, Role::Entry);
    check(fresh.price == e.lattice.back().entry, "Old take frees a current lattice level");
    check(e.used(Side::Short) <= next.shorts.budget, "New and old allocations respect the cap");
}
void limitsAndUncertainty() {
    auto e = setup(1); auto entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, entry.total, true);
    auto take = requireAction(e, Mutation::Create, Role::Take); ack(e, take);
    auto small = e.config; small.shorts.budget = Dollars;
    e.requestRebuild(small); check(!e.nextAction(), "A reduced budget must block new entries");
    check(e.order(take.label)->live(), "Lower budget does not cancel takes");
    e = setup(); entry = requireAction(e, Mutation::Create, Role::Entry);
    e.uncertain(entry.label); check(!e.nextAction(), "Unknown placement may not be retried");
    e.disconnected(); check(!e.nextAction(), "Disconnect freezes mutations until reconciliation");
    ack(e, entry); check(e.orders.size() == 1, "Reconciliation must not duplicate the original order");
}
void switchingAndConflicts() {
    auto e = setup(2, true); auto entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry);
    e.quote(p(83999.9), p(84000.1));
    auto buy = requireAction(e, Mutation::Create, Role::Entry);
    check(buy.buy && buy.price < e.market && e.active == Side::Long, "Price movement permits the waiting long entry without rebuilding"); ack(e, buy);
    check(e.order(entry.label)->live() && !e.rebuilding, "Crossing the boundary preserves the short entry");
    e.quote(p(79999.9), p(80000.1));
    e.pause(); completePause(e);
    e = setup(1); entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, 1, false);
    e.foreign.push_back({"manual", false, e.cycles[0].take});
    check(!e.nextAction() && !e.problem.empty(), "Manual order blocks crossing take");
    check(e.foreign[0].id == "manual", "Manual orders are untouched");
    e.foreign.clear(); auto take = requireAction(e, Mutation::Create, Role::Take); ack(e, take);
    e.cancelAll(); completePause(e); check(e.settled(), "Cancel-all must stop both roles");
    check(e.cycles[0].held == 1, "Cancel-all must not close the position");
}
void denseAndGap() {
    auto e = setup(); e.config.shorts = {p(84500), p(84600.2), 850 * Dollars, 10};
    e.requestRebuild(e.config); check(!e.nextAction() && e.blocked, "Tick-collapsed grids are blocked");
    auto c = setup(2, true).config; c.longs.high = p(84000); c.shorts.low = p(85000);
    e = setup(2, true); e.config = c; check(e.sideAt(p(84500)) == Side::None, "Gap belongs to neither range");
    e.quote(p(84499.9), p(84500.1)); e.requestRebuild(c);
    for (int i = 0; i < 4; ++i) { auto a = requireAction(e, Mutation::Create, Role::Entry); check(a.price <= c.longs.high || a.price >= c.shorts.low, "No entry level may lie inside the gap"); ack(e, a); }
    c.longs.high = p(86000); check(!Engine::validate(c).empty(), "Overlapping ranges are rejected");
}
void simultaneousGridsSurviveFastQuotes() {
    auto e = setup(3, true);
    e.config.longs.high = p(82900); e.config.shorts = {p(82900), p(85000), 850 * Dollars, 3};
    e.quote(p(82905.9), p(82906.1)); e.requestRebuild(e.config);
    for (int i = 0; i < 6; ++i) { auto a = requireAction(e, Mutation::Create, Role::Entry); ack(e, a); }
    check(e.occupiedSlots(Side::Short) == 3 && e.occupiedSlots(Side::Long) == 3, "Both grids exist before crossing the boundary");
    const auto generation = e.generation; const auto orderCount = e.orders.size();
    for (const double mid : {82899.0, 82906.0, 82000.0, 85001.0, 79000.0, 82900.0, 82899.0, 82906.0}) {
        e.quote(p(mid - 0.1), p(mid + 0.1));
        check(!e.rebuilding && e.generation == generation, "Rapid quotes must not change grid generation");
        check(!e.nextAction() && e.orders.size() == orderCount, "Range crossings must not cancel or replace resting entries");
        for (const auto& o : e.orders) check(o.live(), "Both grids keep their existing entries");
    }
    const auto preview = e.preview(e.config);
    check(preview.lattice.size() == 6 && e.generation == generation && e.orders.size() == orderCount, "Preview shows both sides without modifying live state");
    e.pause(); completePause(e); check(e.settled(), "Pause cancels entries of both grids");
}
void takePriorityAndSmallerSlotLimit() {
    auto e = setup(2); auto entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, 1);
    // An old sell entry crosses the new buy take; only that entry may be cancelled.
    e.cycles.push_back({e.nextCycle++, 0, Side::Short, p(84000), p(83000), 1, 0, true});
    Order opposing; opposing.label = "old-short-entry"; opposing.exchangeId = "old-id"; opposing.cycle = e.cycles.back().id;
    opposing.role = Role::Entry; opposing.price = p(84000); opposing.total = 1; opposing.status = Status::Open;
    e.orders.push_back(opposing);
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(cancel.label == opposing.label, "Take must cancel the crossing bot entry first"); ack(e, cancel);
    auto take = requireAction(e, Mutation::Create, Role::Take); ack(e, take);
    auto second = requireAction(e, Mutation::Create, Role::Entry); ack(e, second, 1);
    cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(cancel.label == entry.label, "Adjacent take has priority over the first grid entry"); ack(e, cancel, 1);
    auto secondTake = requireAction(e, Mutation::Create, Role::Take); ack(e, secondTake);
    auto smaller = e.config; smaller.shorts.levels = 1; e.requestRebuild(smaller); completePause(e);
    check(e.occupiedSlots(Side::Short) == 2 && !e.nextAction(), "Old cycles exceeding N must prevent new entries");
    check(e.order(take.label)->live() && e.order(secondTake.label)->live(), "Smaller N preserves both old takes");
}
void manualPositionRecovery() {
    auto e = setup(1); auto entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, 40, true);
    e.pause();
    check(e.trackedPosition() == -40 * e.spec.quantityStep, "Short inventory has a negative sign");
    check(!e.reconcilePosition(0) && e.positionBlocked, "Manually closed position blocks stale inventory on restart");
    check(!e.nextAction(), "A missing old take must never be recreated for a manually closed position");
    const auto sequence = e.nextOrder; e.clearHistory(0);
    check(e.cycles.empty() && e.orders.empty() && e.nextOrder == sequence && !e.positionBlocked, "Reset clears inventory without reusing order labels");
    check(!e.reconcilePosition(7 * e.spec.quantityStep), "Later manual trades cannot silently alter the saved external offset");
    e.clearHistory(7 * e.spec.quantityStep);
    check(e.reconcilePosition(7 * e.spec.quantityStep) && e.trackedPosition() == 0, "Reset keeps an existing manual position outside bot cycles");
    e.requestRebuild(e.config); entry = requireAction(e, Mutation::Create, Role::Entry); ack(e, entry, 1);
    bool refused = false; try { e.clearHistory(0); } catch (const std::exception&) { refused = true; }
    check(refused, "History cannot be cleared with running orders");
    auto take = requireAction(e, Mutation::Create, Role::Take); ack(e, take);
    check(!e.reconcilePosition(0), "Manual change with open takes is detected");
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry); ack(e, cancel, 1);
    check(!e.nextAction() && e.order(take.label)->live(), "Mismatch preserves existing takes and prevents new take creation");
    e.cancelAll(); cancel = requireAction(e, Mutation::Cancel, Role::Take); ack(e, cancel);
    // Reproduce the reported three old ETH long cycles after a manual flatten.
    Engine old; old.profileId = "oldeth"; old.spec = {p(0.01), p(0.0001), {}};
    for (const double level : {2499.8, 2499.6, 2499.4}) old.cycles.push_back({old.nextCycle++, 7, Side::Long, p(level), p(level + 0.2), 40, 40, true});
    old.quote(p(2500.19), p(2500.39));
    check(old.trackedPosition() == p(0.012) && !old.reconcilePosition(0), "Three saved long cycles must match real ETH holdings");
    check(!old.nextAction() && old.orders.empty(), "Startup must not send the three stale marketable takes");
    check(old.reconcilePosition(p(0.012)), "A real retained position may keep its old takes");
    take = requireAction(old, Mutation::Create, Role::Take);
    check(!take.buy && take.total == 40 && take.price == p(2500), "Legitimate old take retains its original price and quantity");
}
void manualTakesKeepPriority() {
    auto e = setup(2, true);
    e.foreign.push_back({"manual-take", false, p(86000), false, true});
    for (int i = 0; i < 2; ++i) {
        auto a = requireAction(e, Mutation::Create, Role::Entry);
        check(a.buy, "A manual sell reduce-only take prevents competing short entries"); ack(e, a);
    }
    check(!e.nextAction() && e.foreign.size() == 1, "Manual take is preserved while conflicting slots wait");
    e = setup(2, true); auto a = requireAction(e, Mutation::Create, Role::Entry); ack(e, a);
    e.foreign.push_back({"late-manual-take", false, p(86000), false, true});
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry); check(cancel.label == a.label, "Cancel the conflicting bot entry, never the manual take"); ack(e, cancel);
    e = setup(1); e.foreign.push_back({"manual-market-trigger", true, 0, true, false});
    check(!e.nextAction() && e.orders.empty(), "An unpriced manual trigger is retained and blocks uncertain conflicts");
}
int main() {
    try {
        pricesAndCaps(); partialAndDuplicate(); partialTakeReentry(); rebuildAndOldSlots();
        limitsAndUncertainty(); switchingAndConflicts(); denseAndGap(); takePriorityAndSmallerSlotLimit(); simultaneousGridsSurviveFastQuotes(); manualPositionRecovery(); manualTakesKeepPriority();
        std::cout << "PASS: " << checks << " grid checks\n"; return EXIT_SUCCESS;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return EXIT_FAILURE; }
}
