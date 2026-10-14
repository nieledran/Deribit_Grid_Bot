#include "trading/controller.h"
#include "exchange/api_numbers.h"
#include <QJsonArray>
#include <algorithm>
#include <stdexcept>

using namespace grid;

void TradeController::execute(const Action& a, Client::Callback completion)
{
    const auto* owned = engine.order(a.label);
    const bool matchesIdentity = owned && a.label.starts_with("dg_" + engine.profileId + "_") &&
                                 owned->exchangeId == a.exchangeId && owned->role == a.role;
    const bool matchesMutation = matchesIdentity && owned->mutation == a.kind &&
                                 owned->price == a.price && engine.buys(*owned) == a.buy;
    const bool matchesQuantity =
        matchesMutation && (a.kind == Mutation::Cancel || owned->desiredTotal == a.total);
    const bool matchesExchangeId =
        matchesQuantity && (a.kind == Mutation::Create || !a.exchangeId.empty());
    const bool belongsToProfile =
        matchesExchangeId &&
        std::none_of(engine.foreign.begin(), engine.foreign.end(), [&](const ForeignOrder& order) {
            return !a.exchangeId.empty() && order.id == a.exchangeId;
        });
    if (!belongsToProfile)
        throw std::runtime_error("Trading request refused: order ownership is not verified");

    // No network mutation is permitted until its intent has committed to SQLite.
    store.save(engine);
    QString method;
    QJsonObject params;
    const auto quantity = static_cast<double>(a.total) * engine.spec.quantityStep / Scale;
    if (a.kind == Mutation::Create) {
        method = a.buy ? "private/buy" : "private/sell";
        params = {{"instrument_name", QString::fromStdString(engine.config.instrument)},
            {"amount", quantity}, {"type", "limit"},
            {"price", static_cast<double>(a.price) / Scale},
            {"time_in_force", "good_til_cancelled"}, {"post_only", false}, {"reduce_only", false},
            {"label", QString::fromStdString(a.label)}};
    } else if (a.kind == Mutation::Edit) {
        method = "private/edit";
        params = {{"order_id", QString::fromStdString(a.exchangeId)}, {"amount", quantity},
            {"price", static_cast<double>(a.price) / Scale}, {"post_only", false}};
    } else if (a.kind == Mutation::Cancel) {
        method = "private/cancel";
        params = {{"order_id", QString::fromStdString(a.exchangeId)}};
    } else
        throw std::runtime_error("Invalid trading mutation");

    client.call(
        method, params,
        [a, completion = std::move(completion)](const RpcResult& response) {
            completion(verifiedResponse(a, response));
        },
        true);
}

RpcResult TradeController::verifiedResponse(const Action& action, const RpcResult& response)
{
    auto result = response;
    if (!result.ok)
        return result;

    const auto root = result.value.toObject();
    const auto order = root.contains("order") ? root["order"].toObject() : root;
    const QString orderId = order["order_id"].toString();
    const bool matchesLabel = order["label"].toString() == QString::fromStdString(action.label);
    const bool hasOrderId = !orderId.isEmpty();
    const bool matchesId =
        action.exchangeId.empty() || orderId == QString::fromStdString(action.exchangeId);
    if (matchesLabel && hasOrderId && matchesId)
        return result;

    result.ok = false;
    result.uncertain = true;
    result.error = "Trading response does not match the requested order";
    return result;
}

std::optional<TradeController::Receipt> TradeController::receiveOwnOrder(
    const QJsonObject& j, bool resolve, bool persist, const std::string* providedLabel)
{
    const auto converted = providedLabel ? std::string{} : j["label"].toString().toStdString();
    const auto& label = providedLabel ? *providedLabel : converted;
    if (!engine.order(label))
        store.recoverOrder(label, engine);
    const auto* o = engine.order(label);
    if (!o)
        return {};
    const auto direction = j["direction"].toString();
    const bool validDirection = direction == "buy" || direction == "sell";
    const bool matchesInstrument =
        j["instrument_name"].toString() == QString::fromStdString(engine.config.instrument) &&
        j["order_type"] == "limit" && validDirection;
    if (!matchesInstrument || (direction == "buy") != engine.buys(*o))
        throw std::runtime_error("Unexpected instrument/type/direction for a bot order");
    const auto status = j["order_state"].toString();
    if (status != "open" && status != "filled" && status != "cancelled" && status != "rejected")
        throw std::runtime_error("Unknown exchange order state");
    const auto timestamp = j["last_update_timestamp"].toInteger(-1);
    if (j["order_id"].toString().isEmpty() || !j["price"].isDouble() || timestamp < 0)
        throw std::runtime_error("Missing bot order fields");
    Update update{label, j["order_id"].toString().toStdString(),
        deribit::apiLots(j["amount"], engine.spec),
        deribit::apiLots(j["filled_amount"], engine.spec), timestamp,
        deribit::fixed(j["price"].toDouble()), status != "open"};
    const bool wasActive = o->mutation == Mutation::None && o->status != Status::Terminal;
    const bool manualCancel = !resolve && update.terminal && status == "cancelled" && wasActive;
    const Int before = o->filled;
    const bool previouslyClosed = o->status == Status::Terminal && o->filled == o->total;
    engine.apply(update);
    if (resolve)
        engine.resolved(label);
    Receipt result;
    result.exchangeId = j["order_id"].toString();
    result.delta = o->filled - before;
    result.previousFilled = before;
    result.take = o->role == Role::Take;
    if (result.take) {
        const auto* c = engine.cycle(o->cycle);
        if (!c)
            throw std::runtime_error("Missing take cycle");
        result.spread = c->side == Side::Long ? c->take - c->entry : c->entry - c->take;
        result.closed = !previouslyClosed && o->status == Status::Terminal && o->filled == o->total;
    }
    if (manualCancel) {
        engine.blocked = true;
        engine.problem = "A bot order was cancelled externally; review and Resume explicitly";
    }
    if (persist)
        store.save(engine);
    return result;
}

std::vector<TradeController::Notification> TradeController::orderedNotifications(
    const QJsonArray& events)
{
    std::array<std::vector<Notification>, 3> classified;
    for (const auto& value : events) {
        const auto json = value.toObject();
        auto label = json["label"].toString().toStdString();
        if (!engine.order(label))
            store.recoverOrder(label, engine);
        const auto* order = engine.order(label);
        classified[!order                       ? 1
                   : order->role == Role::Entry ? 0
                                                : 2]
            .push_back({json, std::move(label), order != nullptr});
    }
    std::vector<Notification> result;
    result.reserve(static_cast<size_t>(events.size()));
    for (auto& group : classified)
        for (auto& order : group)
            result.push_back(std::move(order));
    return result;
}
