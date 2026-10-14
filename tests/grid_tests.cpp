#include "engine/grid.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <source_location>
#include <string>

using namespace grid;
int checks = 0;

void check(bool value, const char* message)
{
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

Int p(double v)
{
    return static_cast<Int>(std::llround(v * Scale));
}

Engine setup(int n = 10, bool both = false)
{
    Engine e;
    e.profileId = "test";
    e.spec = {p(0.1), p(0.0001), {}};
    e.config.longs = {p(80000), p(84500), both ? 850 * Dollars : 0, n};
    e.config.shorts = {p(84500), p(90000), 850 * Dollars, n};
    e.quote(p(84599.9), p(84600.1));
    e.requestRebuild(e.config);
    return e;
}

Update snapshot(const Order& o, Int filled, bool terminal = false, Int time = 1)
{
    return {o.label, o.exchangeId.empty() ? "ex-" + o.label : o.exchangeId, o.total, filled, time,
        o.price, terminal};
}

void verifyTotals(const Engine& e)
{
    for (const auto side : {Side::Long, Side::Short}) {
        Int reserved = 0;
        int occupied = 0, positioned = 0;
        for (const auto& c : e.cycles())
            if (c.side == side) {
                Int lots = c.retired ? c.held : c.capacity;
                bool live = false;
                for (const auto& o : e.orders())
                    if (o.cycle == c.id) {
                        live |= o.live();
                        if (c.retired && o.role == Role::Entry)
                            lots += o.remaining();
                    }
                reserved += e.nominal(lots, c.entry);
                occupied += !c.retired || c.held > 0 || live;
                positioned += c.held > 0;
            }
        check(e.used(side) == reserved, "Cached budget equals independent full traversal");
        check(e.occupiedSlots(side) == occupied,
            "Cached allocation equals independent full traversal");
        const auto& b = side == Side::Long ? e.config.longs : e.config.shorts;
        check(e.availableSlots(side) == std::max(0, b.levels - positioned),
            "Cached position slots equal full traversal");
    }
}

void ack(Engine& e, const Action& a, Int filled = 0, bool terminal = false, Int time = 1)
{
    auto o = *e.order(a.label);
    if (a.kind == Mutation::Edit)
        o.total = a.total;
    auto u = snapshot(o, filled, terminal || a.kind == Mutation::Cancel, time);
    e.apply(u);
    e.resolved(a.label);
    verifyTotals(e);
}

Action requireAction(Engine& e, Mutation kind, Role role,
    std::source_location where = std::source_location::current())
{
    auto a = e.nextAction();
    ++checks;
    if (!a)
        throw std::runtime_error(
            "Expected an action at line " + std::to_string(where.line()) + ": " + e.problem);
    if (a->kind != kind || a->role != role)
        throw std::runtime_error("Unexpected action " + a->label +
                                 ": kind=" + std::to_string(static_cast<int>(a->kind)) +
                                 ", role=" + std::to_string(static_cast<int>(a->role)) +
                                 "; expected kind=" + std::to_string(static_cast<int>(kind)) +
                                 ", role=" + std::to_string(static_cast<int>(role)));
    ++checks;
    return *a;
}

void completePause(Engine& e)
{
    for (int i = 0; i < 100; ++i) {
        auto a = e.nextAction();
        if (!a)
            break;
        const auto order = *e.order(a->label);
        ack(e, *a, order.filled, false, std::max<Int>(50 + i, order.exchangeTime + 1));
    }
}

std::vector<Action> finishActions(Engine& e)
{
    std::vector<Action> actions;
    for (int i = 0; i < 2000; ++i) {
        const auto a = e.nextAction();
        if (!a)
            return actions;
        actions.push_back(*a);
        const auto o = *e.order(a->label);
        ack(e, *a, o.filled, false, o.exchangeTime + 1);
    }
    throw std::runtime_error("Grid did not settle");
}

std::vector<std::pair<Int, Int>> levelsFor(const std::vector<Level>& levels, Side side)
{
    std::vector<std::pair<Int, Int>> result;
    for (const auto& l : levels)
        if (l.side == side)
            result.emplace_back(l.entry, l.take);
    return result;
}

void selectiveSettingsRebuild()
{
    for (const auto side : {Side::Long, Side::Short})
        for (int field = 0; field < 4; ++field) {
            auto e = setup(14, true);
            finishActions(e);
            const auto other = side == Side::Long ? Side::Short : Side::Long;
            const auto originals = e.orders();
            const auto originalCycles = e.cycles();
            const auto levels = levelsFor(e.lattice, other);
            const auto floors = e.rebuildFloor;
            const auto anchors = e.anchors;
            auto next = e.config;
            auto& band = side == Side::Long ? next.longs : next.shorts;
            if (field == 0)
                band.low += side == Side::Long ? -p(1) : p(1);
            if (field == 1)
                band.high += side == Side::Long ? -p(1) : p(1);
            if (field == 2)
                band.budget += 100 * Dollars;
            if (field == 3)
                ++band.levels;
            const auto view = e.preview(next, true);
            check(levelsFor(view.lattice, other) == levels,
                "Selective preview preserves the other lattice");
            check(e.requestRebuild(next, true) == (side == Side::Long ? 1u : 2u),
                "Only the settings-changed side is selected");
            const auto actions = finishActions(e);
            int cancels = 0, creates = 0;
            for (const auto& action : actions) {
                check(e.cycle(e.order(action.label)->cycle)->side == side,
                    "No trading RPC for the unchanged side");
                cancels += action.kind == Mutation::Cancel;
                creates += action.kind == Mutation::Create;
            }
            check(cancels == 14 && creates == band.levels,
                "Apply cancels and replaces only selected entries");
            check(e.running && !e.rebuilding && !e.pendingConfig && !e.rebuildSides,
                "Selective rebuild completes");
            check(
                levelsFor(e.lattice, other) == levels, "Other entry and take levels are unchanged");
            check(
                e.rebuildFloor[static_cast<size_t>(other)] == floors[static_cast<size_t>(other)] &&
                    e.anchors[static_cast<size_t>(other)] == anchors[static_cast<size_t>(other)],
                "Other transition cohort and price anchor survive Apply");
            for (const auto& c : originalCycles)
                if (c.side == other) {
                    const auto* after = e.cycle(c.id);
                    check(after && after->retired == c.retired &&
                              after->generation == c.generation && after->capacity == c.capacity,
                        "Other cycles retain allocation and generation");
                }
            for (const auto& o : originals)
                if (e.cycle(o.cycle)->side == other) {
                    const auto* after = e.order(o.label);
                    check(after && after->exchangeId == o.exchangeId && after->price == o.price &&
                              after->total == o.total && after->status == o.status &&
                              after->mutation == Mutation::None,
                        "Other exchange orders are retained exactly");
                }
            const auto generation = e.generation, nextOrder = e.nextOrder;
            check(e.requestRebuild(e.config, true) == 0 && !e.nextAction(),
                "Unchanged Apply sends no trading requests");
            check(e.generation == generation && e.nextOrder == nextOrder,
                "Unchanged Apply does not restart transition or allocate orders");
            verifyTotals(e);
        }
}

void selectiveRebuildRacesAndTransitions()
{
    for (const auto side : {Side::Long, Side::Short}) {
        auto e = setup(3, true);
        finishActions(e);
        const auto other = side == Side::Long ? Side::Short : Side::Long;
        Order untouched;
        for (const auto& o : e.orders()) {
            if (e.cycle(o.cycle)->side == other && untouched.label.empty())
                untouched = o;
        }
        e.apply(snapshot(untouched, 2, false, 2));
        const auto otherTake = requireAction(e, Mutation::Create, Role::Take);
        ack(e, otherTake);
        const auto oldFloor = e.rebuildFloor[static_cast<size_t>(other)];
        auto next = e.config;
        (side == Side::Long ? next.longs : next.shorts).budget += 100 * Dollars;
        e.requestRebuild(next, true);
        finishActions(e);
        check(e.cycle(untouched.cycle)->held == 2 && !e.cycle(untouched.cycle)->retired &&
                  e.order(otherTake.label)->live(),
            "Unchanged partially filled cycle remains repeatable with its take");
        check(
            e.rebuildFloor[static_cast<size_t>(other)] == oldFloor && e.transitionSlots(other) == 3,
            "Unchanged held cycle does not become an old transition slot");
        const auto same = e.lattice;
        // Closing its take replenishes the original entry, not a new lattice.
        e.apply(snapshot(*e.order(otherTake.label), otherTake.total, true, 10));
        const auto repeat = requireAction(e, Mutation::Edit, Role::Entry);
        check(repeat.label == untouched.label && repeat.total == untouched.total + 2,
            "Unchanged side keeps partial-take repeat behavior");
        ack(e, repeat, untouched.filled + 2, false, 11);
        check(levelsFor(e.lattice, other) == levelsFor(same, other),
            "Other take close does not resize its lattice");
        // A second Apply races a fill, without an artificially crossed opposite take.
        next = e.config;
        (side == Side::Long ? next.longs : next.shorts).budget += 100 * Dollars;
        e.requestRebuild(next, true);
        auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
        check(
            e.cycle(e.order(cancel.label)->cycle)->side == side, "Cancel belongs to selected side");
        ack(e, cancel, 1, true, e.order(cancel.label)->exchangeTime + 1);
        const auto take = requireAction(e, Mutation::Create, Role::Take);
        check(take.price == e.cycle(e.order(cancel.label)->cycle)->take && take.total == 1,
            "Raced fill receives its original take before further cancellation");
        ack(e, take);
        finishActions(e);
        e.apply(snapshot(*e.order(take.label), take.total, true, 10));
        const auto resize = finishActions(e);
        for (const auto& action : resize)
            check(e.cycle(e.order(action.label)->cycle)->side == side,
                "Transition release resizes only its side");
        check(e.transitionSlots(side) == 3 && !e.transitioning(side),
            "Selected old slot is released normally");
        verifyTotals(e);
    }
}

void selectiveRebuildKeepsExistingOppositeTransition()
{
    for (const auto oldSide : {Side::Long, Side::Short}) {
        auto e = setup(3, true);
        finishActions(e);
        Order entry;
        for (const auto& o : e.orders())
            if (e.cycle(o.cycle)->side == oldSide && entry.label.empty())
                entry = o;
        e.apply(snapshot(entry, 2, false, 2));
        const auto take = requireAction(e, Mutation::Create, Role::Take);
        ack(e, take);
        e.requestRebuild(e.config);
        finishActions(e);
        check(e.transitioning(oldSide) && e.transitionSlots(oldSide) == 2,
            "Fixture has an old positioned cycle");
        const auto index = static_cast<size_t>(oldSide);
        const auto floor = e.rebuildFloor[index];
        const auto lattice = levelsFor(e.lattice, oldSide);
        auto next = e.config;
        (oldSide == Side::Long ? next.shorts : next.longs).budget += 100 * Dollars;
        e.requestRebuild(next, true);
        const auto actions = finishActions(e);
        for (const auto& action : actions)
            check(e.cycle(e.order(action.label)->cycle)->side != oldSide,
                "Apply leaves the opposite ongoing transition intact");
        check(e.rebuildFloor[index] == floor && levelsFor(e.lattice, oldSide) == lattice &&
                  e.transitionSlots(oldSide) == 2,
            "Old cycle boundary, denominator and levels survive an opposite Apply");
        e.apply(snapshot(*e.order(take.label), take.total, true, 10));
        const auto resized = finishActions(e);
        for (const auto& action : resized)
            check(e.cycle(e.order(action.label)->cycle)->side == oldSide,
                "Old take release does not resize the other updated side");
        check(!e.transitioning(oldSide) && e.transitionSlots(oldSide) == 3,
            "Unchanged transition completes normally");
    }
}

void selectiveRebuildDisableAndRepeatApply()
{
    auto e = setup(3, true);
    finishActions(e);
    auto next = e.config;
    next.longs.budget = 0;
    e.requestRebuild(next, true);
    auto actions = finishActions(e);
    check(actions.size() == 3 && levelsFor(e.lattice, Side::Long).empty(),
        "Disabling a side cancels only its entries");
    check(e.occupiedSlots(Side::Short) == 3, "Disabling Long retains Short");
    next.longs.budget = 850 * Dollars;
    e.requestRebuild(next, true);
    actions = finishActions(e);
    check(actions.size() == 3 && e.occupiedSlots(Side::Long) == 3,
        "Enabling a side places only its entries");
    next = e.config;
    next.longs.levels = 4;
    e.requestRebuild(next, true);
    const auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel);
    check(e.requestRebuild(e.config, true) == 1,
        "Reverting an in-progress Apply still completes only its accepted side");
    actions = finishActions(e);
    for (const auto& action : actions)
        check(e.cycle(e.order(action.label)->cycle)->side == Side::Long,
            "Repeated Apply retains the other side");
    check(e.occupiedSlots(Side::Long) == 3 && !e.rebuilding,
        "Reverted settings restore the selected allocation");
    next = e.config;
    ++next.longs.levels;
    ++next.shorts.levels;
    check(e.requestRebuild(next, true) == 3, "Changing both sides selects both");
    finishActions(e);
    check(e.occupiedSlots(Side::Long) == 4 && e.occupiedSlots(Side::Short) == 4,
        "Both changed grids receive their new allocations");
    check(e.requestRebuild(e.config) == 3, "Start/Resume remains an explicit full rebuild");
    actions = finishActions(e);
    check(actions.size() == 16, "Full rebuild replaces both sets of entries");
}

void pricesAndCaps()
{
    auto e = setup(10, true);
    auto a = requireAction(e, Mutation::Create, Role::Entry);
    check(!a.buy && a.price == p(85140), "Short grid first price must be 85140");
    check(e.lattice.size() == 20 && e.lattice[9].entry == p(90000) &&
              e.lattice.back().entry == p(80000),
        "Both lattices must cover their ranges");
    check(e.cycles().front().take == p(84600), "First take must be the anchor");
    ack(e, a);
    auto opposite = requireAction(e, Mutation::Create, Role::Entry);
    check(opposite.buy, "Second placement starts the other grid immediately");
    ack(e, opposite);
    for (int i = 0; i < 20; ++i) {
        auto next = e.nextAction();
        if (!next)
            break;
        ack(e, *next);
    }
    check(e.occupiedSlots(Side::Short) == 10, "Must allocate ten slots");
    check(e.occupiedSlots(Side::Long) == 10, "Must also allocate ten long slots");
    check(e.used(Side::Short) <= e.config.shorts.budget, "Budget must not be exceeded");
    check(e.used(Side::Long) <= e.config.longs.budget, "Long budget must not be exceeded");
    check(e.sideAt(p(84500)) == Side::Short, "Boundary belongs to short");
    check(e.sideAt(p(80000)) == Side::Long && e.sideAt(p(90001)) == Side::None, "Range selection");
}

void partialAndDuplicate()
{
    auto e = setup(1);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry);
    const auto o = *e.order(entry.label);
    check(o.total >= 4, "Test needs several lots");
    e.apply(snapshot(o, 2, false, 2));
    e.apply(snapshot(o, 2, false, 2));
    e.apply(snapshot(o, 1, false, 1));
    check(e.cycles()[0].held == 2, "Duplicate and stale fills must be ignored");
    auto take = requireAction(e, Mutation::Create, Role::Take);
    check(take.buy && take.total == 2, "Take must protect exactly the filled lots");
    ack(e, take);
    e.apply(snapshot(o, 4, false, 3));
    auto edit = requireAction(e, Mutation::Edit, Role::Take);
    check(edit.total == 4, "Aggregate another partial fill");
    ack(e, edit, 1, false, 4);
    check(e.cycles()[0].held == 3, "Partial take decreases inventory");
    e.pause();
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel, 5, false, 5);
    check(e.cycles()[0].held == 4, "A fill racing cancellation must be counted");
    edit = requireAction(e, Mutation::Edit, Role::Take);
    check(edit.total == 5, "Edit total includes already filled take quantity");
    ack(e, edit, 1, false, 6);
    check(e.settled(), "Paused inventory must be protected");
}

void partialTakeReentry()
{
    auto e = setup(1);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    const Int q = entry.total;
    ack(e, entry, q, true);
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take);
    e.apply(snapshot(*e.order(take.label), 2, false, 2));
    auto reentry = requireAction(e, Mutation::Create, Role::Entry);
    check(
        reentry.total == 2 && reentry.price == entry.price, "Only the closed fragment may reenter");
    check(e.used(Side::Short) <= e.config.shorts.budget, "Reentry reserves the same budget");
    e = setup(1);
    entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, 4, false);
    take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take, 2, false, 2);
    auto increase = requireAction(e, Mutation::Edit, Role::Entry);
    check(increase.total == entry.total + 2,
        "A closed fragment increases the surviving entry, without duplication");
}

void rebuildAndOldSlots()
{
    auto e = setup(2);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, entry.total, true);
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take);
    auto next = e.config;
    next.shorts.levels = 3;
    e.requestRebuild(next);
    auto fresh = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, fresh);
    check(e.cycles()[0].retired && e.cycles()[0].take == take.price, "Old take must stay fixed");
    check(e.lattice.size() == 2 && e.occupiedSlots(Side::Short) == 2,
        "Old cycle reduces the transition denominator");
    fresh = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, fresh);
    check(!e.nextAction() && e.occupiedSlots(Side::Short) == 3,
        "No extra slots while old take is open");
    e.apply(snapshot(*e.order(take.label), take.total, true, 10));
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel);
    fresh = requireAction(e, Mutation::Create, Role::Entry);
    check(e.lattice.size() == 3 && fresh.price == e.lattice.front().entry,
        "Old take recalculates the step over the full range");
    check(e.used(Side::Short) <= next.shorts.budget, "New and old allocations respect the cap");
}

void rangeTransition()
{
    for (const auto side : {Side::Long, Side::Short}) {
        Engine e;
        e.profileId = "transition";
        e.spec = {p(0.1), p(0.0001), {}};
        auto& band = side == Side::Long ? e.config.longs : e.config.shorts;
        band = {p(82500), p(84500), 850 * Dollars, 14};
        e.quote(p(83500), p(83500));
        for (int i = 0; i < 7; ++i) {
            const Int entry = side == Side::Long ? p(83700 + i) : p(83300 - i);
            const Int take = side == Side::Long ? entry + p(50) : entry - p(50);
            e.importCycle({e.nextCycle++, 0, side, entry, take, 2, 2, true});
        }
        std::vector<Action> oldTakes;
        for (int i = 0; i < 7; ++i) {
            auto a = requireAction(e, Mutation::Create, Role::Take);
            ack(e, a);
            oldTakes.push_back(a);
        }
        e.requestRebuild(e.config);
        completePause(e);
        check(e.transitionSlots(side) == 7 && e.lattice.size() == 7 && e.transitioning(side),
            "Seven free slots use seven intervals");
        const Int endpoint = side == Side::Long ? band.low : band.high;
        check(
            e.lattice.back().entry == endpoint, "Transition immediately reaches the new endpoint");
        const auto generation = e.generation;
        const auto levels = e.lattice;
        const auto current = e.cycles()[7];
        Order currentEntry;
        for (const auto& o : e.orders())
            if (o.cycle == current.id && o.role == Role::Entry)
                currentEntry = o;
        e.apply(snapshot(currentEntry, 2, false, 100));
        auto newTake = requireAction(e, Mutation::Create, Role::Take);
        ack(e, newTake);
        check(!e.nextAction() && e.generation == generation && e.transitionSlots(side) == 7,
            "New entry fill does not resize the transition");
        e.apply(snapshot(*e.order(newTake.label), 1, false, 101));
        completePause(e);
        check(e.generation == generation && e.lattice.front().entry == levels.front().entry,
            "Partial new take uses ordinary reentry without resizing");
        e.apply(snapshot(*e.order(oldTakes[0].label), 1, false, 102));
        completePause(e);
        check(e.generation == generation && e.transitionSlots(side) == 7,
            "Partial old take releases money but not a slot");
        for (int i = 0; i < 7; ++i) {
            // Move toward the entries by less than a step, so only the old take triggers a resize.
            const Int mid = p(side == Side::Long ? 83499 - i : 83501 + i);
            e.quote(mid, mid);
            e.apply(snapshot(*e.order(oldTakes[i].label), 2, true, 110 + i));
            completePause(e);
            check(e.transitionSlots(side) == 8 + i && e.lattice.size() == size_t(8 + i),
                "Each closed old cycle adds one interval");
            check(e.lattice.back().entry == endpoint, "Every transition step covers the endpoint");
            const auto* kept = e.cycle(current.id);
            check(!kept->retired && kept->take == current.take && kept->entry == current.entry,
                "New positioned cycle keeps its take and repeat rules");
            check(e.occupiedSlots(side) == 14 && e.used(side) <= band.budget,
                "Transition preserves slot and budget caps");
            bool liveEndpoint = false;
            for (const auto& o : e.orders())
                if (o.role == Role::Entry && o.live() && o.price == endpoint)
                    liveEndpoint = true;
            check(
                liveEndpoint, "A resting entry reaches the endpoint despite positioned new cycles");
        }
        check(!e.transitioning(side), "Transition ends after all original old cycles close");
        const auto finalGeneration = e.generation;
        e.apply(snapshot(*e.order(newTake.label), 2, true, 200));
        completePause(e);
        check(e.generation == finalGeneration && e.lattice.size() == 14,
            "New take after transition keeps the ordinary grid step");
    }
}

void limitsAndUncertainty()
{
    auto e = setup(1);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, entry.total, true);
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take);
    auto small = e.config;
    small.shorts.budget = Dollars;
    e.requestRebuild(small);
    check(!e.nextAction(), "A reduced budget must block new entries");
    check(e.order(take.label)->live(), "Lower budget does not cancel takes");
    e = setup();
    entry = requireAction(e, Mutation::Create, Role::Entry);
    e.uncertain(entry.label);
    check(!e.nextAction(), "Unknown placement may not be retried");
    e.disconnected();
    check(!e.nextAction(), "Disconnect freezes mutations until reconciliation");
    ack(e, entry);
    check(e.orders().size() == 1, "Reconciliation must not duplicate the original order");
    e = setup(2);
    entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry);
    e.disconnected();
    check(e.running && !e.nextAction(), "Running grid waits for order reconciliation");
    ack(e, entry);
    auto continued = requireAction(e, Mutation::Create, Role::Entry);
    check(continued.label != entry.label,
        "Existing grid continues without duplicating the recovered entry");
    ack(e, continued);
    e.pause();
    e.disconnected();
    check(!e.running, "Disconnect must not undo a manual pause");
    e = setup();
    const auto requested = e.pendingConfig;
    e.disconnected();
    check(e.rebuilding && e.pendingConfig &&
              e.pendingConfig->shorts.budget == requested->shorts.budget &&
              e.pendingConfig->shorts.levels == requested->shorts.levels,
        "Disconnect preserves an accepted rebuild");
}

void switchingAndConflicts()
{
    auto e = setup(2, true);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry);
    if (entry.buy) {
        entry = requireAction(e, Mutation::Create, Role::Entry);
        ack(e, entry);
    }
    e.quote(p(83999.9), p(84000.1));
    auto buy = requireAction(e, Mutation::Create, Role::Entry);
    check(buy.buy && buy.price < e.market && e.active == Side::Long,
        "Price movement permits the waiting long entry without rebuilding");
    ack(e, buy);
    check(e.order(entry.label)->live() && !e.rebuilding,
        "Crossing the boundary preserves the short entry");
    e.quote(p(79999.9), p(80000.1));
    e.pause();
    completePause(e);
    e = setup(1);
    entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, 1, false);
    e.foreign.push_back({"manual", false, e.cycles()[0].take});
    check(!e.nextAction() && !e.problem.empty(), "Manual order blocks crossing take");
    check(e.foreign[0].id == "manual", "Manual orders are untouched");
    e.foreign.clear();
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take);
    e.cancelAll();
    completePause(e);
    check(e.settled(), "Cancel-all must stop both roles");
    check(e.cycles()[0].held == 1, "Cancel-all must not close the position");
}

void denseAndGap()
{
    auto bounded = setup(2, true);
    bounded.config.longs.high = p(84500.05);
    bounded.config.shorts.low = p(84500.05);
    bounded.requestRebuild(bounded.config);
    auto boundedAction = bounded.nextAction();
    check(boundedAction.has_value(), "Non-tick band edges still yield valid levels");
    for (const auto& l : bounded.lattice) {
        const auto& b = l.side == Side::Long ? bounded.config.longs : bounded.config.shorts;
        check(l.entry >= b.low && l.entry <= b.high && l.take >= b.low && l.take <= b.high,
            "Entries and takes both stay inside non-tick bounds");
    }
    auto e = setup();
    e.config.shorts = {p(84500), p(84600.2), 850 * Dollars, 10};
    e.requestRebuild(e.config);
    check(!e.nextAction() && e.blocked, "Tick-collapsed grids are blocked");
    auto c = setup(2, true).config;
    c.longs.high = p(84000);
    c.shorts.low = p(85000);
    e = setup(2, true);
    e.config = c;
    check(e.sideAt(p(84500)) == Side::None, "Gap belongs to neither range");
    e.quote(p(84499.9), p(84500.1));
    e.requestRebuild(c);
    for (int i = 0; i < 4; ++i) {
        auto a = requireAction(e, Mutation::Create, Role::Entry);
        check(a.price <= c.longs.high || a.price >= c.shorts.low,
            "No entry level may lie inside the gap");
        ack(e, a);
    }
    c.longs.high = p(86000);
    check(!Engine::validate(c).empty(), "Overlapping ranges are rejected");
}

void simultaneousGridsSurviveFastQuotes()
{
    auto e = setup(3, true);
    for (int i = 0; i < 6; ++i) {
        auto a = requireAction(e, Mutation::Create, Role::Entry);
        ack(e, a);
    }
    const auto generation = e.generation;
    e.quote(p(84000), p(84000));
    check(!e.rebuilding, "Approaching movement smaller than the short step retains the grid");
    e.quote(p(82700), p(82700));
    check(e.rebuilding, "Short grid follows a full step down");
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(!cancel.buy, "Automatic short rebuild never cancels long entries");
    ack(e, cancel, 0, false, 2);
    e.quote(p(82700.1), p(82700.1));
    check(!e.rebuilding && e.generation == generation,
        "One tick reversal aborts transfer before committing new levels");
    auto restore = requireAction(e, Mutation::Create, Role::Entry);
    check(restore.price == cancel.price && !restore.buy,
        "Cancelled entry is restored at the previous price");
    ack(e, restore);
    e.quote(p(82700), p(82700));
    for (int i = 0; i < 2; ++i) {
        cancel = requireAction(e, Mutation::Cancel, Role::Entry);
        check(!cancel.buy, "Only moving side is cancelled");
        ack(e, cancel, 0, false, 10 + i);
    }
    auto fresh = requireAction(e, Mutation::Create, Role::Entry);
    check(!fresh.buy, "Rebuild places the new short grid");
    ack(e, fresh);
    check(e.generation == generation + 1, "Step move commits one generation");
    for (const auto& l : e.lattice)
        check(l.entry >= (l.side == Side::Long ? e.config.longs.low : e.config.shorts.low) &&
                  l.entry <= (l.side == Side::Long ? e.config.longs.high : e.config.shorts.high),
            "Rebuilt levels remain in range");
    const auto preview = e.preview(e.config);
    check(preview.lattice.size() == 6, "Preview has both grids");
}

void autoRebuildProtectsRacedFills()
{
    Engine e;
    e.profileId = "race";
    e.spec = {p(0.1), p(0.0001), {}};
    e.config.longs = {p(2400), p(2500), 100 * Dollars, 2};
    e.quote(p(2450), p(2450));
    e.requestRebuild(e.config);
    const auto first = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, first);
    const auto endpoint = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, endpoint);
    const auto generation = e.generation;
    e.quote(p(2474.9), p(2474.9));
    check(!e.rebuilding, "Long waits for a complete step");
    e.quote(p(2475), p(2475));
    check(e.rebuilding, "Long follows one step upwards");
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(cancel.label == first.label, "Unchanged endpoint is retained");
    ack(e, cancel, 1, true, 2);
    auto take = requireAction(e, Mutation::Create, Role::Take);
    check(e.generation == generation && take.total == 1 && take.price == p(2450),
        "Raced fill is protected at old take before committing grid");
    ack(e, take);
    check(!e.nextAction() && e.generation == generation + 1,
        "Held old slot and unchanged endpoint reserve the new grid capacity");
    check(e.order(endpoint.label)->live() && e.order(take.label)->price == p(2450),
        "Unchanged entry and existing take are preserved");
    e.apply(snapshot(*e.order(take.label), 1, true, 4));
    verifyTotals(e);
    const auto fresh = requireAction(e, Mutation::Create, Role::Entry);
    check(fresh.price == p(2437.5), "New freed slot uses the latest long anchor");
    e.disconnected();
    check(e.running && !e.nextAction(),
        "Connection loss freezes grid transfer without losing run intent");
    Engine slots;
    slots.profileId = "slots";
    slots.spec = e.spec;
    slots.config.longs = e.config.longs;
    slots.config.longs.levels = 14;
    check(slots.availableSlots(Side::Long) == 14, "Initial display is 14/14");
    for (int i = 0; i < 4; ++i) {
        const auto id = slots.nextCycle++;
        slots.importCycle({id, 0, Side::Long, p(2400), p(2450), 1, 1, true});
        Order o;
        o.label = "slot" + std::to_string(id);
        o.exchangeId = o.label;
        o.cycle = id;
        o.role = Role::Take;
        o.price = p(2450);
        o.total = 1;
        o.status = Status::Open;
        slots.importOrder(o);
    }
    check(slots.availableSlots(Side::Long) == 10, "Four old positioned cycles display 10/14");
    for (int i = 1; i <= 4; ++i)
        slots.apply(snapshot(*slots.order("slot" + std::to_string(i)), 1, true, 1));
    check(slots.availableSlots(Side::Long) == 14, "Closing all old cycles restores 14/14");
    verifyTotals(slots);
}

void lateFillAfterGridCommit()
{
    auto e = setup(2);
    auto first = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, first);
    auto endpoint = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, endpoint);
    e.quote(p(81900), p(81900));
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel, 0, true, 2);
    auto replacement = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, replacement);
    auto old = *e.order(first.label);
    e.apply(snapshot(old, 1, true, 3));
    e.apply(snapshot(old, 1, true, 3));
    auto take = requireAction(e, Mutation::Create, Role::Take);
    check(take.total == 1, "Late archived-generation fill is protected exactly once");
    ack(e, take);
    cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(
        cancel.label == replacement.label, "A late position displaces the nearest opening capacity");
    ack(e, cancel, 0, true, 4);
    check(e.occupiedSlots(Side::Short) <= 2 && e.used(Side::Short) <= e.config.shorts.budget,
        "Late fill returns allocations to the configured caps");
    verifyTotals(e);
}

void malformedIdentityDoesNotMutateInventory()
{
    auto e = setup(2);
    auto first = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, first);
    auto second = requireAction(e, Mutation::Create, Role::Entry);
    const auto order = *e.order(second.label);
    bool refused = false;
    try {
        e.apply(
            {order.label, e.order(first.label)->exchangeId, order.total, 1, 2, order.price, false});
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused && e.cycle(order.cycle)->held == 0 && e.order(order.label)->exchangeId.empty(),
        "Duplicate exchange identity is rejected before changing inventory");
    e.uncertain(order.label);
    check(!e.nextAction(), "Malformed outcome remains frozen for reconciliation");
    verifyTotals(e);
}

void lateFillAfterRestoringSameCycle()
{
    auto e = setup(1);
    auto original = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, original);
    e.pause();
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel, 0, true, 2);
    // Reversal/resume restores the same free cycle before the old final fill is reported.
    e.running = true;
    auto restored = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, restored, restored.total, true, 3);
    const auto old = *e.order(original.label);
    e.apply(snapshot(old, 1, true, 4));
    e.apply(snapshot(old, 1, true, 4));
    const auto* c = e.cycle(old.cycle);
    check(c->held == restored.total + 1 && c->retired && c->capacity == c->held,
        "Overlapping legitimate entry fills are counted and made close-only");
    const auto take = requireAction(e, Mutation::Create, Role::Take);
    check(take.total == restored.total + 1, "Take protects every lot despite late restore race");
    verifyTotals(e);
}

void exactArithmeticAndSlots()
{
    auto e = setup(14);
    check(e.nominal(1, p(82500)) == 8250000, "MSVC exact 8.25 USDC notional");
    check(e.lotsFor(8250000, p(82500)) == 1, "Exact budget purchases one lot");
    check(e.lotsFor(305250370, p(82500.1)) == 37, "Exact boundary purchases 37 lots");
    std::uint64_t rng = 1234567;
    for (int i = 0; i < 10000; ++i) {
        rng = rng * 6364136223846793005ULL + 1;
        const Int price = Int(rng % 10000000000000ULL) + 1;
        const Int budget = Int((rng >> 16) % 10000000000ULL) + 1;
        const Int lots = e.lotsFor(budget, price);
        check(e.nominal(lots, price) <= budget && e.nominal(lots + 1, price) > budget,
            "Maximal lots fit; next lot exceeds budget");
    }
    bool overflow = false;
    try {
        e.nominal(INT64_MAX, INT64_MAX);
    } catch (const std::exception&) {
        overflow = true;
    }
    check(overflow, "Notional overflow is rejected");
    check(e.availableSlots(Side::Short) == 14, "Unfilled entries do not consume position slots");
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, 1);
    check(e.availableSlots(Side::Short) == 13, "Partial entry consumes one position slot");
    e.pause();
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take, 1, true, 3);
    check(e.availableSlots(Side::Short) == 14, "Closing inventory releases its slot");
}

void takePriorityAndSmallerSlotLimit()
{
    auto e = setup(2);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, 1);
    // An old sell entry crosses the new buy take; only that entry may be cancelled.
    e.importCycle({e.nextCycle++, 0, Side::Short, p(84000), p(83000), 1, 0, true});
    Order opposing;
    opposing.label = "old-short-entry";
    opposing.exchangeId = "old-id";
    opposing.cycle = e.cycles().back().id;
    opposing.role = Role::Entry;
    opposing.price = p(84000);
    opposing.total = 1;
    opposing.status = Status::Open;
    e.importOrder(opposing);
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(cancel.label == opposing.label, "Take must cancel the crossing bot entry first");
    ack(e, cancel);
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take);
    auto second = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, second, 1);
    cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(cancel.label == entry.label, "Adjacent take has priority over the first grid entry");
    ack(e, cancel, 1);
    auto secondTake = requireAction(e, Mutation::Create, Role::Take);
    ack(e, secondTake);
    auto smaller = e.config;
    smaller.shorts.levels = 1;
    e.requestRebuild(smaller);
    completePause(e);
    check(e.occupiedSlots(Side::Short) == 2 && !e.nextAction(),
        "Old cycles exceeding N must prevent new entries");
    check(e.order(take.label)->live() && e.order(secondTake.label)->live(),
        "Smaller N preserves both old takes");
}

void manualPositionRecovery()
{
    auto e = setup(1);
    auto entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, 40, true);
    e.pause();
    check(e.trackedPosition() == -40 * e.spec.quantityStep, "Short inventory has a negative sign");
    check(!e.reconcilePosition(0) && e.positionBlocked,
        "Manually closed position blocks stale inventory on restart");
    check(!e.nextAction(),
        "A missing old take must never be recreated for a manually closed position");
    const auto sequence = e.nextOrder;
    e.clearHistory(0);
    check(e.cycles().empty() && e.orders().empty() && e.nextOrder == sequence && !e.positionBlocked,
        "Reset clears inventory without reusing order labels");
    check(!e.reconcilePosition(7 * e.spec.quantityStep),
        "Later manual trades cannot silently alter the saved external offset");
    e.clearHistory(7 * e.spec.quantityStep);
    check(e.reconcilePosition(7 * e.spec.quantityStep) && e.trackedPosition() == 0,
        "Reset keeps an existing manual position outside bot cycles");
    e.requestRebuild(e.config);
    entry = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, entry, 1);
    bool refused = false;
    try {
        e.clearHistory(0);
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "History cannot be cleared with running orders");
    auto take = requireAction(e, Mutation::Create, Role::Take);
    ack(e, take);
    check(!e.reconcilePosition(0), "Manual change with open takes is detected");
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    ack(e, cancel, 1);
    check(!e.nextAction() && e.order(take.label)->live(),
        "Mismatch preserves existing takes and prevents new take creation");
    e.cancelAll();
    cancel = requireAction(e, Mutation::Cancel, Role::Take);
    ack(e, cancel);
    // Reproduce the reported three old ETH long cycles after a manual flatten.
    Engine old;
    old.profileId = "oldeth";
    old.spec = {p(0.01), p(0.0001), {}};
    for (const double level : {2499.8, 2499.6, 2499.4})
        old.importCycle({old.nextCycle++, 7, Side::Long, p(level), p(level + 0.2), 40, 40, true});
    old.quote(p(2500.19), p(2500.39));
    check(old.trackedPosition() == p(0.012) && !old.reconcilePosition(0),
        "Three saved long cycles must match real ETH holdings");
    check(!old.nextAction() && old.orders().empty(),
        "Startup must not send the three stale marketable takes");
    check(old.reconcilePosition(p(0.012)), "A real retained position may keep its old takes");
    take = requireAction(old, Mutation::Create, Role::Take);
    check(!take.buy && take.total == 40 && take.price == p(2500),
        "Legitimate old take retains its original price and quantity");
}

void manualTakesKeepPriority()
{
    auto e = setup(2, true);
    e.foreign.push_back({"manual-take", false, p(86000), false, true});
    for (int i = 0; i < 2; ++i) {
        auto a = requireAction(e, Mutation::Create, Role::Entry);
        check(a.buy, "A manual sell reduce-only take prevents competing short entries");
        ack(e, a);
    }
    check(!e.nextAction() && e.foreign.size() == 1,
        "Manual take is preserved while conflicting slots wait");
    e = setup(2, true);
    auto a = requireAction(e, Mutation::Create, Role::Entry);
    ack(e, a);
    if (a.buy) {
        a = requireAction(e, Mutation::Create, Role::Entry);
        ack(e, a);
    }
    e.foreign.push_back({"late-manual-take", false, p(86000), false, true});
    auto cancel = requireAction(e, Mutation::Cancel, Role::Entry);
    check(cancel.label == a.label, "Cancel the conflicting bot entry, never the manual take");
    ack(e, cancel);
    e = setup(1);
    e.foreign.push_back({"manual-market-trigger", true, 0, true, false});
    check(!e.nextAction() && e.orders().empty(),
        "An unpriced manual trigger is retained and blocks uncertain conflicts");
}

void ordersFollowMarketDistance()
{
    auto e = setup(4, true);
    e.config.longs = {p(800), p(1000), 1000 * Dollars, 4};
    e.config.shorts = {p(1000), p(2000), 1000 * Dollars, 4};
    e.quote(p(1000), p(1000));
    e.requestRebuild(e.config);
    Int previous = 0;
    for (int i = 0; i < 8; ++i) {
        const auto a = requireAction(e, Mutation::Create, Role::Entry);
        const Int distance = std::abs(a.price - e.market);
        check(distance >= previous, "New entries across both sides run from nearest to farthest");
        if (i < 4)
            check(a.buy, "The four nearer long entries precede the farther short grid");
        previous = distance;
        ack(e, a);
    }
    const auto checkCancellations = [](Engine& engine) {
        for (;;) {
            Int nearest = std::numeric_limits<Int>::max();
            for (const auto& o : engine.orders())
                if (o.live() && (engine.cancelEverything || o.role == Role::Entry))
                    nearest = std::min(nearest, std::abs(o.price - engine.market));
            if (nearest == std::numeric_limits<Int>::max())
                break;
            const auto a = engine.nextAction();
            check(a && a->kind == Mutation::Cancel &&
                      std::abs(a->price - engine.market) == nearest,
                "Cancel chooses the nearest live order at the current quote");
            const auto o = *engine.order(a->label);
            ack(engine, *a, o.filled, true, o.exchangeTime + 1);
            engine.quote(p(995), p(995));
        }
    };
    auto rebuilding = e;
    rebuilding.requestRebuild(rebuilding.config);
    checkCancellations(rebuilding);
    auto mismatch = e;
    mismatch.positionBlocked = true;
    checkCancellations(mismatch);
    auto conflict = e;
    conflict.importCycle({100, 0, Side::Short, p(1900), p(1700), 1, 1, true});
    for (const auto price : {p(1250), p(1500)}) {
        const auto a = requireAction(conflict, Mutation::Cancel, Role::Entry);
        check(a.price == price, "Entries crossing a take are cancelled from nearest to farthest");
        ack(conflict, a);
    }
    const auto protectedTake = requireAction(conflict, Mutation::Create, Role::Take);
    check(protectedTake.price == p(1700), "The take is protected before ordinary grid actions");
    auto manual = e;
    manual.foreign.push_back({"manual", false, p(2500), false, true});
    for (const auto price : {p(1250), p(1500), p(1750), p(2000)}) {
        const auto a = requireAction(manual, Mutation::Cancel, Role::Entry);
        check(a.price == price, "Manual-order conflicts also cancel nearest entries first");
        ack(manual, a);
    }
    e.pause();
    checkCancellations(e);

    Engine takes;
    takes.profileId = "distance";
    takes.spec = e.spec;
    takes.quote(p(1000), p(1000));
    takes.importCycle({1, 0, Side::Long, p(900), p(1400), 2, 1, true});
    takes.importCycle({2, 0, Side::Short, p(1300), p(980), 2, 1, true});
    takes.importCycle({3, 0, Side::Long, p(850), p(1010), 2, 2, true});
    Order existing;
    existing.label = "existing-take";
    existing.exchangeId = "existing-id";
    existing.cycle = 3;
    existing.role = Role::Take;
    existing.price = p(1010);
    existing.total = 1;
    existing.status = Status::Open;
    takes.importOrder(existing);
    const auto edit = requireAction(takes, Mutation::Edit, Role::Take);
    check(edit.price == p(1010), "The nearest take edit precedes farther take creates");
    ack(takes, edit);
    takes.quote(p(1450), p(1450));
    const auto farCycle = requireAction(takes, Mutation::Create, Role::Take);
    check(farCycle.price == p(1400), "The next take uses the updated quote");
    ack(takes, farCycle);
    ack(takes, requireAction(takes, Mutation::Create, Role::Take));
    takes.cancelAll();
    checkCancellations(takes);

    Engine repeat;
    repeat.profileId = "repeat";
    repeat.spec = e.spec;
    repeat.config.longs = {p(800), p(1000), 1000 * Dollars, 2};
    repeat.quote(p(1000), p(1000));
    repeat.running = true;
    repeat.lattice = {{p(950), p(1000), Side::Long}, {p(800), p(900), Side::Long}};
    repeat.importCycle({1, 0, Side::Long, p(800), p(900), 1, 0, false});
    repeat.nextCycle = 2;
    const auto nearer = requireAction(repeat, Mutation::Create, Role::Entry);
    check(nearer.price == p(950), "A nearer free level precedes a farther repeat entry");
    ack(repeat, nearer);
    const auto repeated = requireAction(repeat, Mutation::Create, Role::Entry);
    check(repeated.price == p(800), "The existing repeat cycle is then replenished");
}

int main()
{
    try {
        ordersFollowMarketDistance();
        selectiveSettingsRebuild();
        selectiveRebuildRacesAndTransitions();
        selectiveRebuildDisableAndRepeatApply();
        selectiveRebuildKeepsExistingOppositeTransition();
        rangeTransition();
        lateFillAfterRestoringSameCycle();
        malformedIdentityDoesNotMutateInventory();
        lateFillAfterGridCommit();
        autoRebuildProtectsRacedFills();
        exactArithmeticAndSlots();
        pricesAndCaps();
        partialAndDuplicate();
        partialTakeReentry();
        rebuildAndOldSlots();
        limitsAndUncertainty();
        switchingAndConflicts();
        denseAndGap();
        takePriorityAndSmallerSlotLimit();
        simultaneousGridsSurviveFastQuotes();
        manualPositionRecovery();
        manualTakesKeepPriority();
        std::cout << "PASS: " << checks << " grid checks\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
