#include "engine/grid.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

// Price-follow moves and explicit settings changes share the same level builder.
// This file owns selection, transition cohorts and replacement of grid levels.
namespace grid {
namespace {
size_t sideIndex(Side side)
{
    return static_cast<size_t>(side);
}

Int roundUp(Int value, Int tick)
{
    if (value < 0 || tick <= 0)
        throw std::runtime_error("Invalid price/tick");
    const Int remainder = value % tick;
    if (!remainder)
        return value;
    const Int delta = tick - remainder;
    if (value > std::numeric_limits<Int>::max() - delta)
        throw std::runtime_error("Integer overflow");
    return value + delta;
}
}

int Engine::transitionSlots(Side side) const
{
    int slots = band(side).levels;
    for (const auto& c : cycles_)
        if (c.side == side && c.id < rebuildFloor[sideIndex(c.side)] && c.held > 0)
            --slots;
    return std::max(0, slots);
}

bool Engine::transitioning(Side side) const
{
    if (!band(side).budget || rebuildFloor[sideIndex(side)] == 1)
        return false;
    const int slots = transitionSlots(side);
    const auto count = std::count_if(lattice.begin(), lattice.end(), [side](const Level& l) {
        return l.side == side;
    });
    return slots < band(side).levels || count != slots;
}

Engine Engine::movePreview() const
{
    Engine view;
    view.config = config;
    view.spec = spec;
    view.market = market;
    view.rebuildFloor = rebuildFloor;
    for (auto c : cycles_)
        if (c.id < rebuildFloor[sideIndex(c.side)] && c.held > 0) {
            c.retired = true;
            view.importCycle(c);
        }
    view.build();
    return view;
}

bool Engine::prepareMove(Side side, bool resize, const Engine& view)
{
    if (view.blocked) {
        if (resize) {
            blocked = true;
            problem = view.problem;
            ++revision_;
        }
        return false;
    }
    if (market <= 0 || spec.priceTick <= 0 || spec.quantityStep <= 0)
        return false;
    replaceMovingLevels(side, view);
    movingSide_ = side;
    rebuildSides = 1u << sideIndex(side);
    transitionResize_ = resize;
    extreme_ = market;
    rebuilding = true;
    ++revision_;
    return true;
}

void Engine::quote(Int b, Int a)
{
    if (b <= 0 || a < b)
        return;
    bid = b;
    ask = a;
    market = b + (a - b) / 2;
    active = sideAt(market);

    if (movingSide_ != Side::None) {
        updateMovingQuote();
        return;
    }
    const bool canMove = running && !rebuilding && !blocked;
    if (!canMove || positionBlocked || cancelEverything)
        return;

    for (const auto side : {Side::Long, Side::Short}) {
        if (tryFollowPrice(side))
            break;
    }
}

void Engine::updateMovingQuote()
{
    if (transitionResize_)
        return; // A released old slot is not a reversible price-follow move.

    const bool longSide = movingSide_ == Side::Long;
    const Int previousExtreme = extreme_;
    extreme_ = longSide ? std::max(extreme_, market) : std::min(extreme_, market);
    if (extreme_ != previousExtreme)
        refreshMovingLevels();

    const Int tick = spec.tickAt(extreme_);
    if (tick <= 0 || (longSide ? extreme_ - market : market - extreme_) < tick)
        return;

    // Keep the old lattice and cycles: cancelled free entries resume at their old prices.
    movingSide_ = Side::None;
    rebuildSides = 0;
    rebuilding = false;
    running = true;
    ++revision_;
}

void Engine::refreshMovingLevels()
{
    const auto view = movePreview();
    if (view.blocked)
        return;
    replaceMovingLevels(movingSide_, view);
}

void Engine::replaceMovingLevels(Side side, const Engine& view)
{
    movingAnchor_ = view.anchors[sideIndex(side)];
    movingLevels_.clear();
    for (const auto& level : view.lattice) {
        if (level.side == side)
            movingLevels_.push_back(level);
    }
}

Int Engine::moveStep(Side side) const
{
    const auto& range = band(side);
    const Int anchor = anchors[sideIndex(side)];
    const Int end = side == Side::Long ? range.low : range.high;
    const Int distance = std::abs(end - anchor);
    const Int step = std::max(spec.tickAt(anchor), (distance + range.levels - 1) / range.levels);
    for (const auto& level : lattice) {
        if (level.side == side)
            return std::max(spec.tickAt(anchor), std::abs(level.entry - anchor));
    }
    return step;
}

bool Engine::canFollowPrice(Side side) const
{
    const auto& range = band(side);
    const Int anchor = anchors[sideIndex(side)];
    if (!range.budget || !anchor)
        return false;
    if ((side == Side::Long ? market - anchor : anchor - market) < moveStep(side))
        return false;
    // At the outer edge the full-band lattice cannot move any further.
    return std::clamp(market, range.low, range.high) != anchor;
}

bool Engine::tryFollowPrice(Side side)
{
    if (!canFollowPrice(side))
        return false;
    const auto view = movePreview();
    if (view.blocked || !levelsChanged(side, view))
        return false;
    prepareMove(side, false, view);
    return true;
}

bool Engine::levelsChanged(Side side, const Engine& view) const
{
    const auto selected = [side](const Level& level) {
        return level.side == side;
    };
    auto previous = lattice.begin();
    auto next = view.lattice.begin();
    for (;;) {
        previous = std::find_if(previous, lattice.end(), selected);
        next = std::find_if(next, view.lattice.end(), selected);
        if (previous == lattice.end() || next == view.lattice.end())
            return (previous == lattice.end()) != (next == view.lattice.end());
        if (previous->entry != next->entry)
            return true;
        ++previous;
        ++next;
    }
}

unsigned Engine::rebuildSelection(const Config& next, bool onlyChanged) const
{
    const auto changed = [](const Band& a, const Band& b) {
        return a.low != b.low || a.high != b.high || a.budget != b.budget || a.levels != b.levels;
    };
    // Start/Resume and recovery retain their explicit full-rebuild behavior.
    unsigned sides = !onlyChanged || !running || blocked || cancelEverything
                         ? 3u
                         : (changed(config.longs, next.longs) ? 1u : 0u) |
                               (changed(config.shorts, next.shorts) ? 2u : 0u);
    // An Apply during cancellation must finish every already accepted side,
    // even if the user reverts its settings before cancellation has finished.
    if (pendingConfig)
        sides |= rebuildSides;
    return sides;
}

unsigned Engine::requestRebuild(const Config& next, bool onlyChanged)
{
    if (positionBlocked)
        throw std::runtime_error(
            "Position mismatch: reconcile or clear history after cancelling bot orders");
    if (const auto error = validate(next); !error.empty())
        throw std::runtime_error(error);
    const auto sides = rebuildSelection(next, onlyChanged);
    if (!sides)
        return 0;
    movingSide_ = Side::None;
    transitionResize_ = false;
    for (const auto side : {Side::Long, Side::Short})
        if (sides & (1u << sideIndex(side)))
            rebuildFloor[sideIndex(side)] = nextCycle;
    rebuildSides = sides;
    pendingConfig = next;
    rebuilding = true;
    running = sides != 3;
    cancelEverything = false;
    blocked = false;
    problem.clear();
    ++revision_;
    return sides;
}

bool Engine::retainedLevel(const Cycle& c) const
{
    if (transitionResize_ && c.id >= rebuildFloor[sideIndex(c.side)] && c.held > 0)
        return true;
    return std::any_of(movingLevels_.begin(), movingLevels_.end(), [&](const Level& l) {
        return l.entry == c.entry;
    });
}

bool Engine::rebuildCancels(const Cycle& c) const
{
    if (!rebuilding)
        return false;
    if (pendingConfig)
        return (rebuildSides & (1u << sideIndex(c.side))) != 0;
    return movingSide_ == Side::None || (c.side == movingSide_ && !retainedLevel(c));
}

bool Engine::build()
{
    if (market <= 0 || spec.priceTick <= 0 || spec.quantityStep <= 0)
        return false;

    const Side moving = movingSide_;
    const unsigned sides = pendingConfig          ? rebuildSides
                           : moving == Side::None ? 3u
                                                  : (1u << sideIndex(moving));
    retireRebuiltCycles(sides, moving);
    if (moving != Side::None) {
        installMovedLevels();
        return true;
    }

    if (pendingConfig)
        config = *pendingConfig;
    pendingConfig.reset();
    rebuildSides = 0;
    active = sideAt(market);
    std::erase_if(lattice, [sides](const Level& level) {
        return (sides & (1u << sideIndex(level.side))) != 0;
    });
    if (generation == std::numeric_limits<Int>::max())
        throw std::runtime_error("Generation overflow");
    ++generation;

    for (const Side side : {Side::Short, Side::Long}) {
        if (!(sides & (1u << sideIndex(side))))
            continue;
        if (!band(side).budget)
            continue;
        if (!buildSide(side, sides))
            return true;
    }
    movingSide_ = Side::None;
    rebuilding = false;
    running = true;
    ++revision_;
    return true;
}

void Engine::retireRebuiltCycles(unsigned sides, Side moving)
{
    for (auto& cycle : cycles_) {
        if (!(sides & (1u << sideIndex(cycle.side))))
            continue;
        // A positioned cycle becomes close-only; its existing take retains its price.
        if (moving == Side::None || !retainedLevel(cycle))
            cycle.retired = true;
        refreshCycle(cycle.id);
    }
}

void Engine::installMovedLevels()
{
    const Side moving = movingSide_;
    std::erase_if(lattice, [moving](const Level& level) {
        return level.side == moving;
    });
    lattice.insert(lattice.end(), movingLevels_.begin(), movingLevels_.end());
    anchors[sideIndex(moving)] = movingAnchor_;
    if (generation == std::numeric_limits<Int>::max())
        throw std::runtime_error("Generation overflow");
    ++generation;
    movingSide_ = Side::None;
    rebuildSides = 0;
    transitionResize_ = false;
    rebuilding = false;
    running = true;
    ++revision_;
}

bool Engine::buildSide(Side side, unsigned sides)
{
    const auto& range = band(side);
    const Int end = side == Side::Long ? range.low : range.high;
    // Use the live price inside the band; otherwise build the full band from its near edge.
    const Int anchor = market > range.low && market < range.high ? market
                       : side == Side::Long                      ? range.high
                                                                 : range.low;
    anchors[sideIndex(side)] = anchor;
    const Int distance = std::abs(end - anchor);
    const int intervals = transitionSlots(side);
    Int previous = anchor;
    if (distance <= 0)
        return true;

    for (int index = 1; index <= intervals; ++index) {
        const Int price = anchor + (side == Side::Long ? -1 : 1) * (distance * index / intervals);
        const Level level = roundedLevel(side, price, previous);
        const bool validBounds = level.entry >= range.low && level.entry <= range.high;
        const bool validSpread =
            side == Side::Long ? level.entry < level.take : level.entry > level.take;
        if (!validBounds || !validSpread) {
            rejectDenseGrid(side, sides);
            return false;
        }
        lattice.push_back(level);
        previous = level.entry;
    }
    return true;
}

Level Engine::roundedLevel(Side side, Int price, Int previous) const
{
    const auto& range = band(side);
    const Int tick = spec.tickAt(price);
    if (tick <= 0)
        throw std::runtime_error("Invalid price tick");
    price = side == Side::Long ? std::max((price / tick) * tick, roundUp(range.low, tick))
                               : std::min(roundUp(price, tick), (range.high / tick) * tick);
    const Int takeTick = spec.tickAt(previous);
    const Int take = side == Side::Long
                         ? std::min(roundUp(previous, takeTick), (range.high / takeTick) * takeTick)
                         : std::max((previous / takeTick) * takeTick, roundUp(range.low, takeTick));
    return {price, take, side};
}

void Engine::rejectDenseGrid(Side side, unsigned sides)
{
    blocked = true;
    problem = sideName(side) + " grid is too dense for the price tick; reduce N";
    std::erase_if(lattice, [sides](const Level& level) {
        return (sides & (1u << sideIndex(level.side))) != 0;
    });
    movingSide_ = Side::None;
    rebuilding = false;
    running = true;
    ++revision_;
}

Preview Engine::preview(const Config& next, bool onlyChanged) const
{
    if (const auto error = validate(next); !error.empty())
        throw std::runtime_error(error);
    const auto sides = rebuildSelection(next, onlyChanged);
    if (!sides)
        return currentPreview();
    Engine view;
    view.spec = spec;
    view.market = market;
    view.config = config;
    view.rebuildFloor = rebuildFloor;
    view.pendingConfig = next;
    view.rebuildSides = sides;
    view.lattice = lattice;
    view.anchors = anchors;
    for (const auto side : {Side::Long, Side::Short})
        if (sides & (1u << sideIndex(side)))
            view.rebuildFloor[sideIndex(side)] = nextCycle;
    // Copy allocations, never order history; only selected positions become old cycles.
    for (auto c : cycles_) {
        if (sides & (1u << sideIndex(c.side))) {
            if (!c.held)
                continue;
            c.retired = true;
        }
        view.importCycle(c);
    }
    if (!view.build())
        throw std::runtime_error(
            "Connect and wait for instrument specifications and a fresh quote");
    if (view.blocked)
        throw std::runtime_error(view.problem);
    return view.currentPreview();
}

Preview Engine::currentPreview() const
{
    Preview view;
    view.spec = spec;
    view.lattice = lattice;
    view.quantities.reserve(lattice.size());
    view.notionals.reserve(lattice.size());
    view.allocated.reserve(lattice.size());
    std::array<std::unordered_map<Int, Int>, 2> capacities;
    for (const auto& c : cycles_)
        if (!c.retired)
            capacities[sideIndex(c.side)].emplace(c.entry, c.capacity);
    for (const auto side : {Side::Long, Side::Short})
        view.freeSlots[sideIndex(side)] = band(side).levels - occupiedSlots(side);
    for (const auto& l : lattice) {
        const auto index = sideIndex(l.side);
        const auto found = capacities[index].find(l.entry);
        const bool allocated = found != capacities[index].end();
        const Int quantity = allocated ? found->second
                             : view.freeSlots[index] > 0
                                 ? lotsFor(std::max<Int>(0, band(l.side).budget - used(l.side)) /
                                               view.freeSlots[index],
                                       l.entry)
                                 : 0;
        view.quantities.push_back(quantity);
        view.notionals.push_back(nominal(quantity, l.entry));
        view.allocated.push_back(allocated);
    }
    return view;
}
}
