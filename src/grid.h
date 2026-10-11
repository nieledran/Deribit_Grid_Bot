#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace grid {
using Int = std::int64_t;
constexpr Int Scale = 100000000;
constexpr Int Dollars = 1000000;
enum class Side { Long, Short, None };
enum class Role { Entry, Take };
enum class Status { Pending, Open, Uncertain, Terminal, Rejected };
enum class Mutation { None, Create, Edit, Cancel };
struct Band { Int low = 0, high = 0, budget = 0; int levels = 10; };
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
struct ForeignOrder { std::string id; bool buy = false; Int price = 0; bool conditional = false, reduceOnly = false; };
struct Level { Int entry = 0, take = 0; Side side = Side::Short; };
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
    std::vector<Cycle> cycles;
    std::vector<Order> orders;
    std::vector<ForeignOrder> foreign;
    std::vector<Level> lattice;
    Side active = Side::None;
    Int generation = 0, nextCycle = 1, nextOrder = 1;
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
    void requestRebuild(const Config& next);
    void pause();
    void cancelAll();
    void disconnected();
    void resolved(const std::string& label);
    void rejected(const std::string& label, const std::string& reason);
    void uncertain(const std::string& label);
    void apply(const Update& update);
    std::optional<Action> nextAction();
    Engine preview(const Config& next) const;
    bool settled() const;
    Int trackedPosition() const;
    bool positionMatches(Int actual) const;
    bool reconcilePosition(Int actual);
    bool canClearHistory() const;
    void clearHistory(Int actual);
    Int used(Side side) const;
    int occupiedSlots(Side side) const;
    Cycle* cycle(Int id);
    Order* order(const std::string& label);
    bool buys(const Order& order) const;
    Int nominal(Int lots, Int price) const;
    Int lotsFor(Int budget, Int price) const;
private:
    const Band& band(Side side) const;
    Order* liveOrder(Int cycle, Role role);
    bool eligible(bool buy, Int price) const;
    bool crossing(bool buy, Int price, bool foreignOnly = false) const;
    bool entryConflict(bool buy, Int price) const;
    bool build();
    std::optional<Action> create(Cycle& cycle, Role role, Int lots);
    Action mutate(Order& order, Mutation kind, Int total = 0);
};
std::string sideName(Side side);
}
