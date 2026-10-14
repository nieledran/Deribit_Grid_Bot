#include "ui/window.h"
#include "exchange/api_numbers.h"
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;

namespace {
QJsonObject reconciliationSnapshot(const QJsonValue& value)
{
    if (value.isObject())
        return value.toObject();
    if (!value.isArray())
        return {};
    const auto matches = value.toArray();
    return matches.size() == 1 ? matches[0].toObject() : QJsonObject{};
}

QJsonObject findHistoryOrder(const QJsonArray& history, const std::string& label)
{
    QJsonObject match;
    for (const auto& value : history) {
        if (!value.isObject() || value.toObject()["order_id"].toString().isEmpty())
            throw std::runtime_error("Invalid order history record");
        const auto order = value.toObject();
        if (order["label"].toString() != QString::fromStdString(label))
            continue;
        if (!match.isEmpty())
            throw std::runtime_error("Duplicate bot label in order history");
        match = order;
    }
    return match;
}

QJsonObject findOpenOrder(const QJsonArray& orders, const std::string& label)
{
    QJsonObject match;
    for (const auto& value : orders) {
        if (!value.isObject() || value.toObject()["order_id"].toString().isEmpty())
            throw std::runtime_error("Invalid open order record");
        if (value.toObject()["label"].toString() != QString::fromStdString(label))
            continue;
        if (!match.isEmpty())
            throw std::runtime_error("Duplicate bot label in open orders");
        match = value.toObject();
    }
    return match;
}
}

void Window::synchronize()
{
    syncing = true;
    ready = false;
    positionChecks = 0;
    reconcileLabels.clear();
    reconcileIndex = 0;
    reconcileSnapshots.clear();
    absentCreates.clear();
    for (const auto& o : engine.orders())
        if (o.live() || o.mutation != Mutation::None)
            reconcileLabels.push_back(o.label);
    // Classify once; preserve notification order within each role.
    std::stable_partition(
        reconcileLabels.begin(), reconcileLabels.end(), [this](const auto& label) {
            const auto* o = engine.order(label);
            return o && o->role == Role::Entry;
        });
    const int epoch = session;
    client.call("private/get_open_orders_by_instrument",
        {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& r) {
            receiveOpenOrders(r, epoch);
        });
}

void Window::receiveOpenOrders(const RpcResult& r, int epoch)
{
    if (epoch != session)
        return;
    if (!r.ok) {
        fail(r.error);
        return;
    }
    try {
        if (!r.value.isArray())
            throw std::runtime_error("Invalid open order list during reconciliation");
        engine.foreign.clear();
        for (const auto& value : r.value.toArray())
            captureOpenOrder(value.toObject());
        reconcileNext();
    } catch (const std::exception& ex) {
        syncing = false;
        fail(QString::fromUtf8(ex.what()));
    }
}

void Window::captureOpenOrder(const QJsonObject& order)
{
    const QString label = order["label"].toString();
    if (!engine.order(label.toStdString())) {
        if (label.startsWith("dg_" + text(engine.profileId) + "_"))
            throw std::runtime_error(
                "Untracked order with this profile label; manual review required");
        receiveForeignOrder(order);
        return;
    }
    if (reconcileSnapshots.contains(label))
        throw std::runtime_error("Duplicate bot label in open order list");
    reconcileSnapshots.insert(label, order);
}

void Window::reconcileNext()
{
    // Reuse the bulk snapshot in entry-before-take order. Uncertain requests still need an explicit read.
    try {
        while (reconcileIndex < reconcileLabels.size()) {
            const auto& label = reconcileLabels[reconcileIndex];
            const auto* o = engine.order(label);
            if (!o)
                throw std::runtime_error("Missing order during reconciliation");
            const auto snapshot = reconcileSnapshots.find(text(label));
            if (snapshot == reconcileSnapshots.end() || o->exchangeId.empty() ||
                o->mutation != Mutation::None || o->status == Status::Uncertain)
                break;
            // No request is pending for these snapshots; completeSync commits the batch before trading.
            receiveOrder(snapshot.value(), false, false);
            ++reconcileIndex;
        }
    } catch (const std::exception& ex) {
        syncing = false;
        fail(QString::fromUtf8(ex.what()));
        return;
    }
    if (reconcileIndex >= reconcileLabels.size()) {
        completeSync();
        return;
    }
    const auto label = reconcileLabels[reconcileIndex++];
    const auto* o = engine.order(label);
    const int epoch = session;
    if (!o) {
        syncing = false;
        fail("Missing order during reconciliation");
        return;
    }
    const QString method =
        o->exchangeId.empty() ? "private/get_order_state_by_label" : "private/get_order_state";
    const QJsonObject params = o->exchangeId.empty()
                                   ? QJsonObject{{"currency", "USDC"}, {"label", text(label)}}
                                   : QJsonObject{{"order_id", text(o->exchangeId)}};
    client.call(method, params, [this, epoch, label](const RpcResult& r) {
        receiveReconciledOrder(r, epoch, label);
    });
}

void Window::receiveReconciledOrder(const RpcResult& r, int epoch, const std::string& label)
{
    if (epoch != session)
        return;
    // A lost create acknowledgement can mean either an accepted order or a request
    // which never reached the matching engine. Empty recent results alone prove neither.
    if (r.ok && r.value.isArray() && r.value.toArray().isEmpty()) {
        resolveMissingOrder(label);
        return;
    }
    const auto snapshot = reconciliationSnapshot(r.value);
    if (!r.ok || snapshot["order_id"].toString().isEmpty() ||
        snapshot["label"].toString() != text(label)) {
        engine.uncertain(label);
        save();
        syncing = false;
        fail("Cannot resolve saved order " + text(label) + ": " +
             (r.error.isEmpty() ? QString("invalid or ambiguous exchange response; use "
                                          "Reconnect / Reconcile to retry")
                                : r.error));
        return;
    }
    try {
        receiveOrder(snapshot, true);
        reconcileNext();
    } catch (const std::exception& ex) {
        fail(QString::fromUtf8(ex.what()));
        syncing = false;
    }
}

void Window::resolveMissingOrder(const std::string& label)
{
    const auto snapshot = reconcileSnapshots.constFind(text(label));
    if (snapshot != reconcileSnapshots.cend()) {
        try {
            receiveOrder(snapshot.value(), true);
            reconcileNext();
        } catch (const std::exception& ex) {
            syncing = false;
            fail(QString::fromUtf8(ex.what()));
        }
    } else
        reconcileHistory(label, false, 0, 0);
}

void Window::reconcileHistory(const std::string& label, bool historical, int offset, int pass)
{
    const int epoch = session;
    client.call("private/get_order_history_by_instrument",
        {{"instrument_name", text(engine.config.instrument)}, {"count", 1000}, {"offset", offset},
            {"include_old", true}, {"include_unfilled", true}, {"historical", historical}},
        [this, epoch, label, historical, offset, pass](const RpcResult& r) {
            receiveOrderHistory(r, epoch, label, historical, offset, pass);
        });
}

void Window::receiveOrderHistory(
    const RpcResult& r, int epoch, const std::string& label, bool historical, int offset, int pass)
{
    if (epoch != session)
        return;
    try {
        if (!r.ok || !r.value.isArray())
            throw std::runtime_error(
                ("Order history reconciliation failed: " +
                    (r.error.isEmpty() ? QString("invalid exchange response") : r.error))
                    .toStdString());
        const auto history = r.value.toArray();
        const auto match = findHistoryOrder(history, label);
        if (!match.isEmpty()) {
            receiveOrder(match, true);
            reconcileNext();
            return;
        }
        continueHistorySearch(label, historical, offset, pass, history.size());
    } catch (const std::exception& error) {
        syncing = false;
        fail(QString::fromUtf8(error.what()));
    }
}

void Window::continueHistorySearch(
    const std::string& label, bool historical, int offset, int pass, qsizetype count)
{
    if (count == 1000) {
        if (offset >= 99000)
            throw std::runtime_error("Order history search limit reached; missing "
                                     "request remains unresolved");
        reconcileHistory(label, historical, offset + 1000, pass);
        return;
    }
    if (!historical) {
        reconcileHistory(label, true, 0, pass);
        return;
    }
    confirmAbsentCreate(label, pass);
}

void Window::confirmAbsentCreate(const std::string& label, int pass)
{
    const auto* o = engine.order(label);
    if (!o || !o->exchangeId.empty() || o->filled != 0 || o->mutation != Mutation::Create)
        throw std::runtime_error("Saved accepted order is missing from the exchange; "
                                 "manual review required");
    if (pass == 0) {
        confirmMissingCreate(label);
        return;
    }
    // Defer releasing this intent until buffered notifications and account position
    // have also been checked. Its label is never reused for a new request.
    absentCreates.push_back(label);
    reconcileNext();
}

void Window::confirmMissingCreate(const std::string& label)
{
    const int epoch = session;
    log("No exchange record for pending create " + text(label) +
        "; checking again before releasing its slot");
    QTimer::singleShot(2000, this, [this, epoch, label] {
        readMissingCreate(epoch, label);
    });
}

void Window::readMissingCreate(int epoch, const std::string& label)
{
    if (epoch != session)
        return;
    client.call("private/get_open_orders_by_instrument",
        {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch, label](const RpcResult& r) {
            receiveMissingCreate(r, epoch, label);
        });
}

void Window::receiveMissingCreate(const RpcResult& r, int epoch, const std::string& label)
{
    if (epoch != session)
        return;
    try {
        if (!r.ok || !r.value.isArray())
            throw std::runtime_error(
                "Cannot confirm open orders for missing create: " + r.error.toStdString());
        const auto match = findOpenOrder(r.value.toArray(), label);
        if (!match.isEmpty()) {
            receiveOrder(match, true);
            reconcileNext();
        } else {
            reconcileHistory(label, false, 0, 1);
        }
    } catch (const std::exception& error) {
        syncing = false;
        fail(QString::fromUtf8(error.what()));
    }
}

void Window::completeSync()
{
    try {
        receiveOrders(bufferedOrders);
        bufferedOrders = {};
        const int epoch = session;
        const auto revision = positionRevision;
        client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}},
            [this, epoch, revision](const RpcResult& r) {
                receiveSyncPosition(r, epoch, revision);
            });
    } catch (const std::exception& e) {
        fail(QString::fromUtf8(e.what()));
    }
}

void Window::receiveSyncPosition(const RpcResult& r, int epoch, quint64 revision)
{
    if (epoch != session)
        return;
    if (!r.ok) {
        syncing = false;
        fail("Position reconciliation failed: " + r.error);
        return;
    }
    // Drain batched order notifications before comparing the account snapshot.
    QTimer::singleShot(200, this, [this, epoch, revision, r] {
        finishSyncPosition(r, epoch, revision);
    });
}

void Window::finishSyncPosition(const RpcResult& r, int epoch, quint64 revision)
{
    if (epoch != session)
        return;
    try {
        receiveOrders(bufferedOrders);
        bufferedOrders = {};
        if (positionRevision != revision) {
            completeSync();
            return;
        }
        netPosition = accountPosition(r.value.toObject());
        const Int actual = signedFixed(netPosition);
        // Do not adopt an unexplained position as the initial external offset
        // when deciding that an uncertain create was not accepted.
        if (!absentCreates.empty() &&
            (!engine.positionMatches(actual) ||
                (!engine.externalPosition && actual != engine.trackedPosition()))) {
            syncing = false;
            fail("Missing create cannot be released: account position does not "
                 "match saved fills; use Reconnect / Reconcile after review");
            return;
        }
        if (!engine.positionMatches(actual) && positionChecks++ < 2) {
            QTimer::singleShot(250, this, [this, epoch] {
                if (epoch == session)
                    completeSync();
            });
            return;
        }
        const bool matched = engine.reconcilePosition(actual);
        releaseAbsentCreates(matched);
        finishReconciliation(matched, actual);
    } catch (const std::exception& e) {
        syncing = false;
        fail(QString::fromUtf8(e.what()));
    }
}

void Window::releaseAbsentCreates(bool matched)
{
    if (!matched) {
        absentCreates.clear();
        return;
    }
    for (const auto& label : absentCreates) {
        const auto* o = engine.order(label);
        if (!o)
            throw std::runtime_error("Missing create during final reconciliation");
        if (!o->exchangeId.empty()) {
            engine.resolved(label);
            continue;
        }
        if (o->filled != 0 || o->mutation != Mutation::Create)
            throw std::runtime_error("Missing create changed during reconciliation");
        engine.rejected(label, "Unaccepted create reconciled; review and Start / Resume");
        log("Pending create absent from open orders and recent/historical "
            "records; released " +
            text(label) + ". Start / Resume is available.");
    }
    absentCreates.clear();
}

void Window::finishReconciliation(bool matched, grid::Int actual)
{
    if (!matched)
        log("Position mismatch: " + positionDetails(engine, actual));
    if (!statsReady)
        feesSince = QDateTime::currentMSecsSinceEpoch();
    syncing = false;
    ready = true;
    statsReady = true;
    save();
    log(matched
            ? QString("Reconciled own orders and account position. %1.")
                  .arg(engine.running ? "Running" : "Paused; Start / Resume rebuilds free slots")
            : text(engine.problem));
    receiveTrades(bufferedTrades);
    bufferedTrades = {};
    // Protect recovered fills before queuing fee/account reads or waiting for the next pump tick.
    if (matched)
        pump();
    try {
        for (const auto& id : store->missingFees())
            refreshFees(id);
    } catch (const std::exception&) {
        feesUnavailable = true;
    }
    refreshAccount();
    render();
}
