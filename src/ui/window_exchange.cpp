#include "ui/window.h"
#include "exchange/api_numbers.h"
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;

void Window::initialize(const QJsonObject&)
{
    ++session;
    const int epoch = session;
    syncing = true;
    ready = false;
    bufferedOrders = {};
    bufferedTrades = {};
    feeRequests.clear();

    log("Authenticated; checking instrument, account and cancel-on-disconnect");
    loadInstrument(epoch);
}

void Window::loadInstrument(int epoch)
{
    client.call("public/get_instrument", {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch](const RpcResult& response) {
            receiveInstrument(response, epoch);
        });
}

void Window::receiveInstrument(const RpcResult& response, int epoch)
{
    if (epoch != session)
        return;
    if (!response.ok) {
        fail(response.error);
        return;
    }
    try {
        const auto instrument = response.value.toObject();
        const auto next = instrumentSpec(instrument);
        engine.spec = next;
        save();
        log(QString("Instrument: tick=%1, quantity step=%2 coin, contract size=%3 coin")
                .arg(instrument["tick_size"].toDouble(), 0, 'g', 12)
                .arg(instrument["min_trade_amount"].toDouble(), 0, 'g', 12)
                .arg(instrument["contract_size"].toDouble(), 0, 'g', 12));

        bindAccount(epoch);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

grid::Spec Window::instrumentSpec(const QJsonObject& instrument) const
{
    const QString baseCurrency = engine.config.instrument.starts_with("BTC") ? "BTC" : "ETH";
    const bool matchesProfile =
        instrument["instrument_name"].toString() == text(engine.config.instrument) &&
        instrument["settlement_currency"].toString() == "USDC" &&
        instrument["base_currency"].toString() == baseCurrency;
    const bool tradablePerpetual = matchesProfile && instrument["kind"].toString() == "future" &&
                                   instrument["settlement_period"].toString() == "perpetual" &&
                                   instrument["state"].toString() == "open";
    if (!tradablePerpetual)
        throw std::runtime_error("Instrument is not a tradable USDC linear perpetual");

    Spec next{fixed(instrument["tick_size"].toDouble()),
        fixed(instrument["min_trade_amount"].toDouble()), {}};
    if (next.priceTick <= 0 || next.quantityStep <= 0)
        throw std::runtime_error("Invalid instrument specification");
    if (!engine.cycles().empty() && engine.spec.quantityStep &&
        engine.spec.quantityStep != next.quantityStep)
        throw std::runtime_error("Quantity step changed; existing state requires manual review");

    for (const auto& value : instrument["tick_size_steps"].toArray()) {
        const auto step = value.toObject();
        if (!step["above_price"].isDouble() || !step["tick_size"].isDouble() ||
            step["tick_size"].toDouble() <= 0)
            throw std::runtime_error("Invalid tick step specification");
        next.tickSteps.emplace_back(
            fixed(step["above_price"].toDouble()), fixed(step["tick_size"].toDouble()));
    }

    return next;
}

void Window::bindAccount(int epoch)
{
    client.call("private/get_account_summary", {{"currency", "USDC"}, {"extended", true}},
        [this, epoch](const RpcResult& response) {
            if (epoch != session)
                return;
            if (!response.ok) {
                fail(response.error);
                return;
            }
            try {
                const auto account = response.value.toObject();
                const auto id = account["id"];
                client.setTradingLimits(account["limits"].toObject());
                const bool stringId = id.isString() && !id.toString().isEmpty();
                const bool numericId = id.isDouble() && id.toInteger() > 0;
                if (!stringId && !numericId)
                    throw std::runtime_error(
                        "Account ID is missing from the extended account summary");
                const QString accountId =
                    id.isString() ? id.toString() : QString::number(id.toInteger());

                const QString identity = accountId + "/" +
                                         (engine.config.production ? "prod/" : "test/") +
                                         text(engine.config.instrument);
                profiles->bindAccount(identity);

                disableCancelOnDisconnect(epoch);
            } catch (const std::exception& error) {
                fail(QString::fromUtf8(error.what()));
                client.stop();
            }
        });
}

void Window::disableCancelOnDisconnect(int epoch)
{
    client.call("private/disable_cancel_on_disconnect", {{"scope", "connection"}},
        [this, epoch](const RpcResult& response) {
            if (epoch != session)
                return;
            if (!response.ok) {
                fail("Cannot disable connection cancel-on-disconnect: " + response.error);
                return;
            }
            subscribeToUpdates(epoch);
        });
}

void Window::subscribeToUpdates(int epoch)
{
    const QString instrument = text(engine.config.instrument);
    const QJsonArray channels{
        "ticker." + instrument + ".100ms",
        "user.changes." + instrument + ".100ms",
    };

    client.call(
        "private/subscribe", {{"channels", channels}}, [this, epoch](const RpcResult& response) {
            if (epoch != session)
                return;
            if (!response.ok) {
                fail(response.error);
                return;
            }
            loadInitialTicker(epoch);
        });
}

void Window::loadInitialTicker(int epoch)
{
    client.call("public/ticker", {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch](const RpcResult& response) {
            if (epoch != session)
                return;
            if (!response.ok) {
                fail(response.error);
                return;
            }
            try {
                readMarket(response.value.toObject());
                synchronize();
            } catch (const std::exception& error) {
                fail(QString::fromUtf8(error.what()));
            }
        });
}

void Window::readMarket(const QJsonObject& ticker)
{
    if (ticker["instrument_name"].toString() != text(engine.config.instrument))
        return;
    if (!ticker["best_bid_price"].isDouble() || !ticker["best_ask_price"].isDouble())
        return;
    const Int b = fixed(ticker["best_bid_price"].toDouble()),
              a = fixed(ticker["best_ask_price"].toDouble());
    if (b <= 0 || a < b)
        return;
    engine.quote(b, a);
    marketClock.restart();
}

void Window::refreshAccount()
{
    const bool canRead = ready && !syncing && !readBusy;
    if (!canRead || !client.authenticated())
        return;

    readBusy = true;
    const int epoch = session;
    const auto revision = positionRevision;
    client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch, revision](const RpcResult& response) {
            receiveAccountPosition(response, epoch, revision);
        });
}

void Window::receiveAccountPosition(const RpcResult& response, int epoch, quint64 revision)
{
    if (epoch != session)
        return;
    try {
        Int actual = 0;
        if (response.ok) {
            netPosition = accountPosition(response.value.toObject());
            actual = signedFixed(netPosition);
        } else {
            log(response.error);
        }

        const auto finish = [this, epoch, revision, actual, ok = response.ok] {
            finishAccountRead(epoch, revision, actual, ok);
        };
        // Account reads can overtake the aggregated fill notification; do not freeze takes on that first snapshot.
        const bool currentSnapshot = response.ok && revision == positionRevision;
        if (currentSnapshot && !engine.positionMatches(actual))
            QTimer::singleShot(200, this, finish);
        else
            finish();
    } catch (const std::exception& error) {
        readBusy = false;
        log(QString::fromUtf8(error.what()));
        return;
    }
}

void Window::finishAccountRead(int epoch, quint64 revision, Int actual, bool ok)
{
    if (epoch != session)
        return;
    try {
        const bool canReconcile = ready && !syncing && !busy && !engine.positionBlocked;
        const bool currentSnapshot = ok && canReconcile && revision == positionRevision;
        if (currentSnapshot && !engine.positionMatches(actual)) {
            readBusy = false;
            log("Account position differs from tracked fills; reconciling: " +
                positionDetails(engine, actual));
            synchronize();
            return;
        }

        refreshTicker(epoch);
    } catch (const std::exception& error) {
        readBusy = false;
        log(QString::fromUtf8(error.what()));
    }
}

void Window::refreshTicker(int epoch)
{
    client.call("public/ticker", {{"instrument_name", text(engine.config.instrument)}},
        [this, epoch](const RpcResult& response) {
            if (epoch != session)
                return;
            readBusy = false;
            try {
                if (response.ok)
                    readMarket(response.value.toObject());
                else
                    log(response.error);
                render();
            } catch (const std::exception& error) {
                fail(QString::fromUtf8(error.what()));
            }
        });
}
