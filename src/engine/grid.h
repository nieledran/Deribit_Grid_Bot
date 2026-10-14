#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>

namespace grid {
using Int = std::int64_t;
constexpr Int Scale = 100000000;
constexpr Int Dollars = 1000000;
enum class Side { Long, Short, None };
enum class Role { Entry, Take };
enum class Status { Pending, Open, Uncertain, Terminal, Rejected };
enum class Mutation { None, Create, Edit, Cancel };

struct Band {
    Int low = 0, high = 0, budget = 0;
    int levels = 10;
};

struct Config {
    std::string instrument = "BTC_USDC-PERPETUAL";
    bool production = false;
    Band longs, shorts;
};

struct Spec {
    Int priceTick = 0, quantityStep = 0;
    std::vector<std::pair<Int, Int>> tickSteps;
    Int tickAt(Int price) const;
};

struct Cycle {
    Int id = 0, generation = 0;
    Side side = Side::Long;
    Int entry = 0, take = 0, capacity = 0, held = 0;
    bool retired = false;
};

struct Order {
    std::string label, exchangeId;
    Int cycle = 0;
    Role role = Role::Entry;
    Int price = 0, total = 0, filled = 0;
    Int exchangeTime = 0;
    Status status = Status::Pending;
    Mutation mutation = Mutation::None;
    Int desiredTotal = 0;
    bool live() const;
    Int remaining() const;
};

struct ForeignOrder {
    std::string id;
    bool buy = false;
    Int price = 0;
    bool conditional = false, reduceOnly = false;
};

struct Level {
    Int entry = 0, take = 0;
    Side side = Side::Short;
};

struct Preview {
    Spec spec;
    std::vector<Level> lattice;
    std::vector<Int> quantities, notionals;
    std::vector<bool> allocated;
    std::array<int, 2> freeSlots{};
};

struct Action {
    Mutation kind = Mutation::None;
    std::string label, exchangeId;
    bool buy = false;
    Role role = Role::Entry;
    Int price = 0, total = 0;
};

struct Update {
    std::string label, exchangeId;
    Int total = 0, filled = 0, time = 0, price = 0;
    bool terminal = false;
};

// The engine never sends requests. The caller durably saves an action before sending it.
class Engine {
public:
    Config config;
    Spec spec;
    std::string profileId;

    const std::vector<Cycle>& cycles() const
    {
        return cycles_;
    }

    const std::vector<Order>& orders() const
    {
        return orders_;
    }

    void restoreRecords(std::vector<Cycle> cycles, std::vector<Order> orders);
    void importCycle(Cycle cycle);
    void importOrder(Order order);

    std::uint64_t revision() const
    {
        return revision_;
    }

    std::vector<ForeignOrder> foreign;
    std::vector<Level> lattice;
    Side active = Side::None;
    Int generation = 0, nextCycle = 1, nextOrder = 1, historyFloor = 1;
    // Each side keeps its own cohort when Apply changes only the other side.
    std::array<Int, 2> rebuildFloor{1, 1};
    std::array<Int, 2> anchors{};
    unsigned rebuildSides = 0; // Pending explicit rebuild: Long=1, Short=2.
    Int market = 0, bid = 0, ask = 0;
    bool running = false, rebuilding = false, cancelEverything = false;
    bool blocked = false;
    bool positionBlocked = false;
    std::optional<Int> externalPosition;
    std::string problem;
    std::optional<Config> pendingConfig;

    static std::string validate(const Config& config);
    Side sideAt(Int price) const;
    void quote(Int bestBid, Int bestAsk);
    unsigned requestRebuild(const Config& next, bool onlyChanged = false);
    void pause();
    void cancelAll();
    void disconnected();
    void resolved(const std::string& label);
    void rejected(const std::string& label, const std::string& reason);
    void uncertain(const std::string& label);
    void apply(const Update& update);
    std::optional<Action> nextAction();
    Preview preview(const Config& next, bool onlyChanged = false) const;
    Preview currentPreview() const;
    bool settled(unsigned sides = 3) const;
    Int trackedPosition() const;
    bool positionMatches(Int actual) const;
    bool reconcilePosition(Int actual);
    bool canClearHistory() const;
    void clearHistory(Int actual);
    Int used(Side side) const;
    int occupiedSlots(Side side) const;
    int availableSlots(Side side) const;
    int transitionSlots(Side side) const;
    bool transitioning(Side side) const;
    const Cycle* cycle(Int id) const;
    const Order* order(const std::string& label) const;
    const Order* orderByExchangeId(const std::string& id) const;
    bool hasLiveOrders(Int cycle) const;
    bool buys(const Order& order) const;
    Int nominal(Int lots, Int price) const;
    Int lotsFor(Int budget, Int price) const;

private:
    std::vector<Cycle> cycles_;
    std::vector<Order> orders_;
    std::unordered_map<Int, size_t> cycleIndex_;
    std::unordered_map<std::string, size_t> labelIndex_, exchangeIndex_;
    std::unordered_set<size_t> liveIndices_;

    struct Totals {
        Int reserved = 0;
        int occupied = 0, positioned = 0;
    };

    struct Links {
        std::optional<size_t> entry, take;
        Totals contribution;
    };

    std::unordered_map<Int, Links> links_;
    std::array<Totals, 2> totals_{};
    std::uint64_t revision_ = 0;
    Side movingSide_ = Side::None;
    bool transitionResize_ = false;
    Int extreme_ = 0, movingAnchor_ = 0;
    std::vector<Level> movingLevels_;
    bool retainedLevel(const Cycle& cycle) const;
    bool rebuildCancels(const Cycle& cycle) const;
    unsigned rebuildSelection(const Config& next, bool onlyChanged) const;
    Engine movePreview() const;
    bool prepareMove(Side side, bool resize, const Engine& view);
    void updateMovingQuote();
    void refreshMovingLevels();
    void replaceMovingLevels(Side side, const Engine& view);
    Int moveStep(Side side) const;
    bool canFollowPrice(Side side) const;
    bool tryFollowPrice(Side side);
    bool levelsChanged(Side side, const Engine& view) const;
    void refreshCycle(Int id);
    void refreshOrder(size_t index);
    Cycle* mutableCycle(Int id);
    Order* mutableOrder(const std::string& label);
    const Band& band(Side side) const;
    Order* liveOrder(Int cycle, Role role);
    bool eligible(bool buy, Int price) const;
    bool crossing(bool buy, Int price, bool foreignOnly = false) const;
    bool entryConflict(bool buy, Int price) const;
    bool build();
    void retireRebuiltCycles(unsigned sides, Side moving);
    void installMovedLevels();
    bool buildSide(Side side, unsigned sides);
    Level roundedLevel(Side side, Int price, Int previous) const;
    void rejectDenseGrid(Side side, unsigned sides);
    std::optional<Action> create(Cycle& cycle, Role role, Int lots);
    Action mutate(Order& order, Mutation kind, Int total = 0);
    bool hasUnresolvedOrders() const;
    std::optional<Action> cancelRequestedOrders();
    Cycle* unprotectedCycle();
    std::optional<Action> protectCycle(Cycle& cycle);
    std::optional<Action> clearTakeConflict(const Cycle& cycle, bool buy);
    std::optional<Action> nextGridAction();
    void resizeTransition();
    void resizeReleasedSlots();
    std::optional<Action> cancelRetiredEntries();
    bool finishRebuild();
    std::optional<Action> nextEntryAction();
    std::optional<Action> retireExcessCapacity();
    std::optional<Action> cancelConflictingEntry();
    Cycle* repeatEntry();
    std::optional<Action> allocateEntry(const Level& level);
    int offGridAllocations(Side side) const;
    bool levelAllocated(const Level& level) const;
    const Level* freeEntryLevel(Side side);
};

std::string sideName(Side side);
}
