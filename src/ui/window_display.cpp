#include "ui/window.h"
#include "ui/rebuild_button.h"
#include <QDateTime>
#include <stdexcept>

using namespace grid;

namespace {
QString money(Int value)
{
    return QString::number(static_cast<double>(value) / Dollars, 'f', 2);
}

QString state(Status s)
{
    switch (s) {
    case Status::Pending:
        return "Pending";
    case Status::Open:
        return "Open";
    case Status::Uncertain:
        return "Uncertain";
    case Status::Terminal:
        return "Closed";
    case Status::Rejected:
        return "Rejected";
    }
    return "Invalid";
}

QString engineMode(const Engine& engine)
{
    const bool blocked = engine.positionBlocked || engine.cancelEverything || engine.blocked;
    if (blocked) {
        if (engine.positionBlocked)
            return "Position mismatch";
        if (engine.cancelEverything)
            return "Cancelling all";
        return "Blocked";
    }

    if (engine.rebuilding && !engine.running)
        return "Rebuilding";
    return engine.running ? "Running" : "Paused";
}

void cell(QTableWidget* table, int row, int col, const QString& value)
{
    auto* item = table->item(row, col);
    if (!item)
        table->setItem(row, col, new QTableWidgetItem(value));
    else if (item->text() != value)
        item->setText(value);
}

}

QString Window::text(const std::string& s)
{
    return QString::fromStdString(s);
}

double Window::price(Int value)
{
    return static_cast<double>(value) / Scale;
}

QString Window::amount(Int lots, const Spec& s)
{
    return QString::number(static_cast<double>(lots) * s.quantityStep / Scale, 'f', 8);
}

QString Window::role(Role r)
{
    return r == Role::Entry ? "Entry" : "Take";
}

void Window::log(const QString& message)
{
    journal->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + message);
}

void Window::updateLatency()
{
    const auto status = client.connectionStatus();
    using State = Client::ConnectionStatus::State;
    QString label = "Latency: — ms";
    if (status.state == State::Offline)
        label = "Latency: offline";
    else if (status.state == State::Connecting)
        label = QString("Connecting: %1 ms").arg(status.milliseconds);
    else if (status.milliseconds >= 0)
        label =
            QString("Latency: %1%2 ms").arg(status.waiting ? "≥ " : "").arg(status.milliseconds);
    if (latencyLabel->text() != label)
        latencyLabel->setText(label);
    const QString style = status.slow || status.state == State::Offline ? "color:#b91c1c" : "";
    if (latencyLabel->styleSheet() != style)
        latencyLabel->setStyleSheet(style);
}

void Window::render()
{
    if (!summaryLabel)
        return;

    renderControls();
    renderSummary();
    renderCycles();
    renderOrders();
    renderGridPreview();
}

void Window::renderControls()
{
    updateLatency();
    const bool idle = !busy && !storageFailed && !closing;
    const bool synchronized = ready && !syncing;
    const bool canOperate = synchronized && idle;
    const bool freshQuote = canOperate && !engine.positionBlocked && marketClock.isValid() &&
                            marketClock.elapsed() <= 10000;

    connectButton->setEnabled(idle);
    connectButton->setText(client.authenticated() ? "Reconnect / Reconcile" : "Connect");
    startButton->setEnabled(freshQuote);
    resetButton->setEnabled(canOperate && engine.canClearHistory());
    applyButton->setEnabled(startButton->isEnabled());
    pauseButton->setEnabled(ready || engine.running || engine.rebuilding);
    const bool canCancel = client.authenticated() && profiles->hasAccountLock() && idle;
    cancelButton->setEnabled(canCancel);
    deleteProfileButton->setEnabled(canCancel);
    idBox->setEnabled(!client.authenticated());
    secretBox->setEnabled(!client.authenticated());
    connectionLabel->setText(syncing                  ? "Reconciling"
                             : ready                  ? "Connected"
                             : client.authenticated() ? "Needs reconciliation"
                                                      : "Disconnected");
}

void Window::renderSummary()
{
    QString mode = engineMode(engine);
    QStringList transitions;
    for (const auto side : {Side::Long, Side::Short})
        if (engine.transitioning(side)) {
            const auto& range = side == Side::Long ? engine.config.longs : engine.config.shorts;
            transitions.append(QString("%1 %2/%3")
                    .arg(text(sideName(side)))
                    .arg(engine.transitionSlots(side))
                    .arg(range.levels));
        }
    if (!transitions.isEmpty())
        mode += " [transition: " + transitions.join(", ") + "]";
    applyButton->setTransition(!transitions.isEmpty());
    bool feesComplete = false;
    try {
        feesComplete = !feesUnavailable && store->feesComplete();
    } catch (const std::exception&) {
        feesUnavailable = true;
    }
    statsLabel->setText(
        QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC%3\nClosed trades: %4")
            .arg(sessionPnl, 0, 'f', 4)
            .arg(sessionFees, 0, 'f', 4)
            .arg(feesComplete ? "" : " (incomplete)")
            .arg(closedTrades));
    const QString grids = engine.config.longs.budget && engine.config.shorts.budget ? "Long + Short"
                          : engine.config.longs.budget                              ? "Long"
                          : engine.config.shorts.budget                             ? "Short"
                                                                                    : "Disabled";
    summaryLabel->setText(
        QString("%1 | Grids: %2 | Mid: %3 | Net position: %4 coin\nLong reserved: %5 / %6 USDC (%7 "
                "/ %8 slots) | Short reserved: %9 / %10 USDC (%11 / %12 slots)%13")
            .arg(mode, grids, QString::number(price(engine.market), 'f', 4),
                QString::number(netPosition, 'f', 8))
            .arg(money(engine.used(Side::Long)), money(engine.config.longs.budget))
            .arg(engine.availableSlots(Side::Long))
            .arg(engine.config.longs.levels)
            .arg(money(engine.used(Side::Short)), money(engine.config.shorts.budget))
            .arg(engine.availableSlots(Side::Short))
            .arg(engine.config.shorts.levels)
            .arg(engine.problem.empty() ? "" : "\n" + text(engine.problem)));
}

void Window::renderCycles()
{
    int cycleRow = 0;
    for (const auto& c : engine.cycles()) {
        if (c.retired && c.held == 0 && !engine.hasLiveOrders(c.id))
            continue;
        const int row = cycleRow++;
        if (row >= cyclesTable->rowCount())
            cyclesTable->insertRow(row);
        const QStringList values{QString::number(c.id), text(sideName(c.side)),
            QString::number(c.generation), QString::number(price(c.entry), 'f', 4),
            QString::number(price(c.take), 'f', 4), amount(c.capacity, engine.spec),
            amount(c.held, engine.spec),
            money(engine.nominal(c.retired ? c.held : c.capacity, c.entry)),
            c.retired ? "Old: close only" : "Current: repeat"};
        for (int col = 0; col < values.size(); ++col)
            cell(cyclesTable, row, col, values[col]);
    }
    cyclesTable->setRowCount(cycleRow);
}

void Window::renderOrders()
{
    int orderRow = 0;
    for (const auto& o : engine.orders()) {
        if (!o.live() && o.mutation == Mutation::None && o.status != Status::Rejected)
            continue;
        const int row = orderRow++;
        if (row >= ordersTable->rowCount())
            ordersTable->insertRow(row);
        const QStringList values{QString::number(o.cycle),
            role(o.role) + (engine.buys(o) ? " / Buy" : " / Sell"),
            QString::number(price(o.price), 'f', 4), amount(o.total, engine.spec),
            amount(o.filled, engine.spec), state(o.status), text(o.exchangeId)};
        for (int col = 0; col < values.size(); ++col)
            cell(ordersTable, row, col, values[col]);
    }
    for (const auto& f : engine.foreign) {
        const int row = orderRow++;
        if (row >= ordersTable->rowCount())
            ordersTable->insertRow(row);
        cell(ordersTable, row, 0, "Other / Manual");
        cell(ordersTable, row, 1,
            (f.buy ? QString("Buy") : QString("Sell")) + (f.conditional ? " / Trigger" : ""));
        cell(ordersTable, row, 2,
            f.price > 0 ? QString::number(price(f.price), 'f', 4) : "Market on trigger");
        cell(ordersTable, row, 3, "");
        cell(ordersTable, row, 4, "");
        cell(ordersTable, row, 5, "Untouched");
        cell(ordersTable, row, 6, text(f.id));
    }
    ordersTable->setRowCount(orderRow);
}

void Window::renderGridPreview()
{
    if (previewBox->isChecked()) {
        try {
            renderPreview(engine.preview(uiConfig(), true));
        } catch (const std::exception& e) {
            previewTable->setRowCount(1);
            for (int col = 0; col < previewTable->columnCount(); ++col)
                cell(previewTable, 0, col, "");
            cell(previewTable, 0, 5, QString::fromUtf8(e.what()));
        }
    } else
        renderPreview(engine.currentPreview());
}

void Window::renderPreview(const Preview& view)
{
    previewTable->setRowCount(static_cast<int>(view.lattice.size()));
    int index = 0;
    for (const auto& l : view.lattice) {
        const bool existing = view.allocated[static_cast<size_t>(index)];
        const int free = view.freeSlots[static_cast<size_t>(l.side)];
        const Int q = view.quantities[static_cast<size_t>(index)];
        cell(previewTable, index, 0, text(sideName(l.side)) + " / " + QString::number(index + 1));
        cell(previewTable, index, 1, QString::number(price(l.entry), 'f', 4));
        cell(previewTable, index, 2, QString::number(price(l.take), 'f', 4));
        cell(previewTable, index, 3, amount(q, view.spec));
        cell(previewTable, index, 4, money(view.notionals[static_cast<size_t>(index)]));
        cell(previewTable, index, 5,
            existing   ? "Allocated"
            : free > 0 ? "Waiting / Available"
                       : "Waiting for a slot");
        ++index;
    }
}

QString Window::positionDetails(const Engine& engine, Int actual)
{
    const Int tracked = engine.trackedPosition();
    const Int external = engine.externalPosition.value_or(tracked == 0 ? actual : 0);
    const double expected = price(tracked) + price(external);
    return QString("actual=%1 coin; bot=%2 coin; external=%3 coin; expected=%4 coin; difference=%5 "
                   "coin")
        .arg(price(actual), 0, 'f', 8)
        .arg(price(tracked), 0, 'f', 8)
        .arg(price(external), 0, 'f', 8)
        .arg(expected, 0, 'f', 8)
        .arg(price(actual) - expected, 0, 'f', 8);
}
