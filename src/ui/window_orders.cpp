#include "ui/window.h"
#include "exchange/api_numbers.h"
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;

void Window::receiveOrder(
    const QJsonObject& json, bool resolve, bool persist, const std::string* label)
{
    const auto receipt = controller->receiveOwnOrder(json, resolve, persist, label);
    if (!receipt) {
        receiveForeignOrder(json);
        return;
    }
    if (receipt->delta > 0)
        ++positionRevision;
    if (receipt->delta > 0) {
        // The first reconciliation counts only fills beyond the saved order quantity.
        if (!statsReady && !recoveredFeeSkips.contains(receipt->exchangeId))
            recoveredFeeSkips.insert(receipt->exchangeId, receipt->previousFilled);
        try {
            store->expectFee(receipt->exchangeId, receipt->delta);
        } catch (const std::exception&) {
            feesUnavailable = true;
        }
        QTimer::singleShot(250, this, [this, id = receipt->exchangeId] {
            refreshFees(id);
        });
    }
    if (receipt->take) {
        sessionPnl += price(receipt->spread) * static_cast<double>(receipt->delta) *
                      engine.spec.quantityStep / Scale;
        if (receipt->closed)
            ++closedTrades;
    }
}

void Window::receiveForeignOrder(const QJsonObject& j)
{
    if (j["instrument_name"].toString() != text(engine.config.instrument))
        return;
    const auto id = j["order_id"].toString().toStdString();
    if (id.empty())
        return;
    std::erase_if(engine.foreign, [&](const ForeignOrder& f) {
        return f.id == id;
    });
    const auto status = j["order_state"].toString();
    if (status == "open" || status == "untriggered" || status == "triggered") {
        const bool conditional = j["order_type"].toString() != "limit";
        const Int p = j["price"].isDouble() ? fixed(j["price"].toDouble()) : 0;
        engine.foreign.push_back(
            {id, j["direction"].toString() == "buy", p, conditional, j["reduce_only"].toBool()});
    } else if (status == "cancelled") {
        log("Other / Manual order cancelled: " + text(id) +
            " | Exchange reason: " + j["cancel_reason"].toString("not provided"));
    }
}

void Window::receiveOrders(const QJsonArray& events)
{
    for (const auto& order : controller->orderedNotifications(events)) {
        if (order.own)
            receiveOrder(order.json, false, false, &order.label);
        else
            receiveForeignOrder(order.json);
    }
    // Commit a notification batch once; every later trading mutation still saves its intent first.
    save();
}

void Window::receiveTrades(const QJsonArray& trades, Int skipLots)
{
    if (!statsReady)
        return;
    const bool recovered = skipLots >= 0;
    for (const auto& value : trades) {
        const auto t = value.toObject();
        const QString id = t["order_id"].toString();
        const bool validTrade = !id.isEmpty() &&
                                t["instrument_name"].toString() == text(engine.config.instrument) &&
                                t["timestamp"].isDouble();
        const bool belongsToPeriod =
            validTrade && (recovered || t["timestamp"].toInteger() >= feesSince);
        if (!belongsToPeriod)
            continue;
        const auto* labelled = engine.order(t["label"].toString().toStdString());
        bool own = false;
        try {
            own =
                (labelled && (labelled->exchangeId.empty() || text(labelled->exchangeId) == id)) ||
                engine.orderByExchangeId(id.toStdString()) != nullptr || store->ownsExchangeId(id);
        } catch (const std::exception&) {
            feesUnavailable = true;
            continue;
        }
        const QString tradeId = t["trade_id"].toString();
        const QString key = id + "/" + tradeId;
        if (!own || tradeId.isEmpty())
            continue;
        Int lots = 0, totalLots = 0;
        try {
            lots = totalLots = apiLots(t["amount"], engine.spec);
            if (lots <= 0)
                continue;
            if (recovered) {
                // Order history is requested oldest first; omit the already saved fills.
                const Int skipped = std::min(skipLots, lots);
                skipLots -= skipped;
                lots -= skipped;
                if (lots == 0)
                    continue;
            }
        } catch (const std::exception&) {
            feesUnavailable = true;
            continue;
        }
        if (!t["fee"].isDouble() || t["fee_currency"].toString() != "USDC" ||
            !std::isfinite(t["fee"].toDouble()))
            continue;
        // Fee data must never change inventory or prevent trading when unavailable/malformed.
        try {
            if (store->recordFee(key, id, lots))
                sessionFees += t["fee"].toDouble() * static_cast<double>(lots) / totalLots;
        } catch (const std::exception&) {
            feesUnavailable = true;
        }
    }
}

void Window::refreshFees(const QString& id)
{
    try {
        const bool canRead = !feesUnavailable && ready && !syncing && client.authenticated();
        if (!canRead || feeRequests.contains(id) ||
            store->feeValue(id, true) >= store->feeValue(id))
            return;
        feeRequests.insert(id);
        const int epoch = session;
        const Int expected = store->feeValue(id);
        const auto statsEpoch = statsRevision;
        client.call("private/get_user_trades_by_order", {{"order_id", id}, {"sorting", "asc"}},
            [this, id, epoch, expected, statsEpoch](const RpcResult& r) {
                receiveFeeTrades(r, id, epoch, expected, statsEpoch);
            });
    } catch (const std::exception&) {
        feesUnavailable = true;
    }
}

void Window::receiveFeeTrades(
    const RpcResult& r, const QString& id, int epoch, grid::Int expected, quint64 statsEpoch)
{
    if (epoch != session || statsEpoch != statsRevision)
        return;
    try {
        feeRequests.remove(id);
        if (r.ok && r.value.isArray())
            receiveTrades(r.value.toArray(), recoveredFeeSkips.value(id, -1));
        else
            log("Fees unavailable for " + id + "; gross P&L remains available");
        render();
        // A fill received during this read needs a newer snapshot, but missing fees alone do not cause retries.
        if (store->feeValue(id) > expected)
            refreshFees(id);
    } catch (const std::exception&) {
        feesUnavailable = true;
        feeRequests.remove(id);
    }
}

void Window::resetStats()
{
    try {
        store->resetFees();
        ++statsRevision;
        feesSince = QDateTime::currentMSecsSinceEpoch();
        feeRequests.clear();
        recoveredFeeSkips.clear();
        sessionPnl = 0;
        sessionFees = 0;
        closedTrades = 0;
        feesUnavailable = false;
        log("P&L, fees and closed trades reset. New statistics period started.");
        render();
    } catch (const std::exception& e) {
        log("Cannot reset statistics: " + QString::fromUtf8(e.what()));
    }
}
