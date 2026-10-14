#include "profile/profile_controller.h"
#include "exchange/api_numbers.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QUuid>
#include <limits>
#include <stdexcept>

using namespace grid;
using namespace deribit;

namespace {
QString text(const std::string& value)
{
    return QString::fromStdString(value);
}
}

ProfileController::ProfileController(Engine& engine, Store& store, Client& client,
    const int& session, std::unique_ptr<QLockFile> profileLock, QObject* parent)
    : QObject(parent), engine(engine), store(store), client(client), session(session),
      profileLock(std::move(profileLock))
{
    connect(&client, &Client::lost, this, &ProfileController::finishCancellation);
}

void ProfileController::bindAccount(const QString& identity)
{
    store.bindAccount(identity);
    const auto hash =
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex();
    if (!accountLock) {
        accountLock = std::make_unique<QLockFile>(
            QDir::tempPath() + "/deribit-grid-" + QString::fromLatin1(hash) + ".lock");
        if (!accountLock->tryLock()) {
            accountLock.reset();
            throw std::runtime_error("Another local bot is using this account/instrument");
        }
    }
}

bool ProfileController::hasAccountLock() const
{
    return bool(accountLock);
}

bool ProfileController::cancelling() const
{
    return cancellationActive;
}

void ProfileController::cancelOrders(bool eraseProfile, int epoch)
{
    cancellationActive = true;
    client.call("private/get_open_orders_by_instrument",
        {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch, eraseProfile](const RpcResult& response) {
            receiveCancellationOrders(response, epoch, eraseProfile);
        });
}

void ProfileController::receiveCancellationOrders(
    const RpcResult& response, int epoch, bool eraseProfile)
{
    if (epoch != session)
        return;
    try {
        cancelProfileLabels(cancellationLabels(response), eraseProfile);
    } catch (const std::exception& error) {
        finishCancellation();
        emit failed(QString::fromUtf8(error.what()) + "; profile retained", true);
    }
}

QMap<QString, Int> ProfileController::cancellationLabels(const RpcResult& response) const
{
    if (!response.ok || !response.value.isArray())
        throw std::runtime_error(
            "Cannot list orders for cancellation: " + response.error.toStdString());
    QMap<QString, Int> labels;
    for (const auto& order : engine.orders())
        if (order.live() || order.mutation != Mutation::None)
            labels.insert(text(order.label), order.price);
    const auto prefix = "dg_" + text(engine.profileId) + "_";
    for (const auto& value : response.value.toArray()) {
        if (!value.isObject())
            throw std::runtime_error("Invalid open order list");
        const auto order = value.toObject();
        if (order["instrument_name"].toString() != text(engine.config.instrument))
            throw std::runtime_error("Wrong instrument in cancellation snapshot");
        const auto label = order["label"].toString();
        if (label.startsWith(prefix)) {
            const auto price = order["price"];
            // Retain labels without a known limit price so cancellation can still finish.
            const Int knownPrice = price.isDouble() ? fixed(price.toDouble()) : labels.value(label);
            labels.insert(label, knownPrice);
        }
    }
    return labels;
}

void ProfileController::cancelProfileLabels(QMap<QString, Int> labels, bool eraseProfile)
{
    if (labels.isEmpty()) {
        verifyProfileCancellation(eraseProfile, 0);
        return;
    }
    auto selected = labels.begin();
    const auto distance = [this](Int price) {
        return price > 0 ? std::abs(price - engine.market) : std::numeric_limits<Int>::max();
    };
    for (auto it = labels.begin(); it != labels.end(); ++it)
        if (distance(it.value()) < distance(selected.value()))
            selected = it;
    const auto label = selected.key();
    labels.erase(selected);
    const int epoch = session;
    client.call(
        "private/cancel_by_label", {{"currency", "USDC"}, {"label", label}},
        [this, epoch, labels, eraseProfile](const RpcResult& response) {
            if (epoch != session)
                return;
            if (!response.ok || !response.value.isDouble() || response.value.toInteger(-1) < 0) {
                finishCancellation();
                emit failed("Label cancellation not confirmed: " +
                                (response.error.isEmpty() ? QString("invalid exchange response")
                                                          : response.error) +
                                "; profile retained. Reconnect / Reconcile, or retry Cancel all "
                                "bot orders.",
                    true);
                return;
            }
            cancelProfileLabels(labels, eraseProfile);
        },
        true);
}

void ProfileController::verifyProfileCancellation(bool eraseProfile, int checks)
{
    const int epoch = session;
    // A second empty snapshot catches an order accepted just after the first cancel.
    QTimer::singleShot(checks ? 2000 : 0, this, [this, epoch, eraseProfile, checks] {
        readCancellationSnapshot(epoch, eraseProfile, checks);
    });
}

void ProfileController::readCancellationSnapshot(int epoch, bool eraseProfile, int checks)
{
    if (epoch != session)
        return;

    client.call("private/get_open_orders_by_instrument",
        {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch, eraseProfile, checks](const RpcResult& response) {
            receiveCancellationSnapshot(response, epoch, eraseProfile, checks);
        });
}

void ProfileController::receiveCancellationSnapshot(
    const RpcResult& response, int epoch, bool eraseProfile, int checks)
{
    if (epoch != session)
        return;

    try {
        validateCancellationSnapshot(response);

        if (checks == 0) {
            verifyProfileCancellation(eraseProfile, 1);
            return;
        }
        if (!eraseProfile) {
            finishCancellation();
            emit cancelled();
            return;
        }

        recreateProfile(epoch);
    } catch (const std::exception& error) {
        finishCancellation();
        emit failed(QString::fromUtf8(error.what()) + "; profile retained", true);
    }
}

void ProfileController::validateCancellationSnapshot(const RpcResult& response) const
{
    if (!response.ok || !response.value.isArray())
        throw std::runtime_error(
            "Final cancellation check failed: " + response.error.toStdString());

    for (const auto& value : response.value.toArray()) {
        if (!value.isObject())
            throw std::runtime_error("Invalid final cancellation snapshot");

        const auto order = value.toObject();
        const bool matchesInstrument =
            order["instrument_name"].toString() == text(engine.config.instrument);
        const bool hasOrderId = !order["order_id"].toString().isEmpty();
        if (!matchesInstrument || !hasOrderId)
            throw std::runtime_error("Invalid final cancellation snapshot");

        const bool belongsToProfile =
            order["label"].toString().startsWith("dg_" + text(engine.profileId) + "_");
        if (belongsToProfile)
            throw std::runtime_error("A profile order is still open; retry Cancel all bot orders");
    }
}

void ProfileController::recreateProfile(int epoch)
{
    client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch](const RpcResult& response) {
            receiveRecreationPosition(response, epoch);
        });
}

void ProfileController::receiveRecreationPosition(const RpcResult& response, int epoch)
{
    if (epoch != session)
        return;
    try {
        if (!response.ok)
            throw std::runtime_error(
                "Cannot read remaining position: " + response.error.toStdString());

        Engine fresh;
        fresh.config.instrument = engine.config.instrument;
        fresh.config.production = engine.config.production;
        fresh.spec = engine.spec;
        fresh.profileId = QUuid::createUuid().toString(QUuid::Id128).left(16).toStdString();
        const double position = accountPosition(response.value.toObject());
        emit positionRead(position);
        fresh.externalPosition = signedFixed(position);

        store.resetProfile(fresh);
        engine = std::move(fresh);
        finishCancellation();
        emit profileRecreated();
    } catch (const std::exception& error) {
        finishCancellation();
        emit failed(QString::fromUtf8(error.what()) + "; profile retained", true);
    }
}

void ProfileController::finishCancellation()
{
    cancellationActive = false;
}

void ProfileController::resetHistory(int epoch)
{
    client.call("private/get_open_orders_by_instrument",
        {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch](const RpcResult& response) {
            receiveHistoryOrders(response, epoch);
        });
}

void ProfileController::receiveHistoryOrders(const RpcResult& response, int epoch)
{
    if (epoch != session)
        return;
    if (!response.ok || !response.value.isArray()) {
        emit failed("Cannot verify bot order cancellation: " + response.error, false);
        return;
    }

    const QString prefix = "dg_" + text(engine.profileId) + "_";
    for (const auto& value : response.value.toArray()) {
        if (value.toObject()["label"].toString().startsWith(prefix)) {
            emit failed(
                "Bot orders remain on the exchange; cancel and reconcile before resetting", false);
            return;
        }
    }

    readResetPosition(epoch);
}

void ProfileController::readResetPosition(int epoch)
{
    client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch](const RpcResult& response) {
            receiveResetPosition(response, epoch);
        });
}

void ProfileController::receiveResetPosition(const RpcResult& response, int epoch)
{
    if (epoch != session)
        return;
    emit historyReadFinished();
    try {
        if (!response.ok)
            throw std::runtime_error("Cannot verify account position before resetting");

        const double position = accountPosition(response.value.toObject());
        emit positionRead(position);
        engine.clearHistory(signedFixed(position));
        emit historyCleared();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()), false);
    }
}
