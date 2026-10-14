#include "ui/window.h"
#include "ui/rebuild_button.h"
#include "exchange/api_numbers.h"
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;

namespace {
void cell(QTableWidget* table, int row, int col, const QString& value)
{
    auto* item = table->item(row, col);
    if (!item)
        table->setItem(row, col, new QTableWidgetItem(value));
    else if (item->text() != value)
        item->setText(value);
}

void setupTable(QTableWidget* table, const QStringList& headers)
{
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
}

}

void Window::setupUi()
{
    setWindowTitle("Deribit Grid — " + text(engine.config.instrument) + " — " +
                   QFileInfo(profilePath).baseName());
    resize(1250, 850);
    auto* root = new QWidget;
    auto* layout = new QVBoxLayout(root);
    setCentralWidget(root);

    setupConnectionControls(layout);
    setupTradingControls(layout);
    setupTablesAndTabs(layout);
    setupHistoryAndJournal(layout);
    setupFooter(layout);

    // Instrument and environment belong to the selected profile.
    instrumentBox->setEnabled(false);
    environmentBox->setEnabled(false);
    connect(instrumentBox, &QComboBox::currentTextChanged, this, [this] {
        render();
    });
}

void Window::setupConnectionControls(QVBoxLayout* layout)
{
    auto* connection = new QHBoxLayout;

    instrumentBox = new QComboBox;
    instrumentBox->addItems({"BTC_USDC-PERPETUAL", "ETH_USDC-PERPETUAL"});

    environmentBox = new QComboBox;
    environmentBox->addItems({"Testnet", "Production"});

    idBox = new QLineEdit;
    idBox->setPlaceholderText("Client ID");
    idBox->setObjectName("clientId");

    secretBox = new QLineEdit;
    secretBox->setEchoMode(QLineEdit::Password);
    secretBox->setPlaceholderText("Client secret (memory only)");
    secretBox->setObjectName("clientSecret");

    connectButton = new QPushButton("Connect");
    connectButton->setObjectName("connect");

    connectionLabel = new QLabel("Disconnected");
    connectionLabel->setObjectName("connectionStatus");

    connection->addWidget(instrumentBox);
    connection->addWidget(environmentBox);
    connection->addWidget(new QLabel("Client ID:"));
    connection->addWidget(idBox);
    connection->addWidget(new QLabel("Secret:"));
    connection->addWidget(secretBox);
    connection->addWidget(connectButton);
    connection->addWidget(connectionLabel);
    layout->addLayout(connection);
    connect(connectButton, &QPushButton::clicked, this, &Window::connectExchange);
}

void Window::setupTradingControls(QVBoxLayout* layout)
{
    setupSettings(layout);
    setupGridControls(layout);
    setupSummary(layout);
}

void Window::setupSettings(QVBoxLayout* layout)
{
    settings = new QTableWidget(2, 5);
    settings->setMaximumHeight(118);
    settings->setHorizontalHeaderLabels({"Side", "Lower bound (USDC)", "Upper bound (USDC)",
        "Entry notional cap (USDC)", "Intervals / slots"});
    settings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    settings->verticalHeader()->hide();
    for (int row = 0; row < 2; ++row) {
        cell(settings, row, 0, row == 0 ? "Long" : "Short");
        settings->item(row, 0)->setFlags(Qt::ItemIsEnabled);
        for (int col = 1; col <= 3; ++col) {
            auto* spin = new QDoubleSpinBox;
            spin->setRange(0, col == 3 ? 1e9 : 1e7);
            spin->setDecimals(col == 3 ? 2 : 8);
            spin->setSingleStep(col == 3 ? 10 : 1);
            spin->setGroupSeparatorShown(true);
            settings->setCellWidget(row, col, spin);
        }
        auto* n = new QSpinBox;
        n->setRange(1, 200);
        settings->setCellWidget(row, 4, n);
    }
    layout->addWidget(settings);
    setConfig(engine.config);
}

void Window::setupGridControls(QVBoxLayout* layout)
{
    auto* buttons = new QHBoxLayout;

    startButton = new QPushButton("Start / Resume");
    startButton->setObjectName("start");

    pauseButton = new QPushButton("Pause (keep takes)");
    pauseButton->setObjectName("pause");

    applyButton = new RebuildButton("Apply settings / Rebuild");
    applyButton->setObjectName("applySettings");

    cancelButton = new QPushButton("Cancel all bot orders");
    cancelButton->setObjectName("cancelBotOrders");

    previewBox = new QCheckBox("Preview pending settings");

    auto* resetStatsButton = new QPushButton("Reset stats");
    resetStatsButton->setObjectName("resetStats");
    resetStatsButton->setToolTip("Start a new count of P&L, fees and closed trades. Trading, "
                                 "orders and positions continue unchanged.");
    connect(resetStatsButton, &QPushButton::clicked, this, &Window::resetStats);

    buttons->addWidget(startButton);
    buttons->addWidget(pauseButton);
    buttons->addWidget(applyButton);
    buttons->addWidget(cancelButton);
    buttons->addWidget(previewBox);
    buttons->addStretch();
    buttons->addWidget(resetStatsButton);
    layout->addLayout(buttons);
    connect(startButton, &QPushButton::clicked, this, [this] {
        startGrid();
    });
    connect(applyButton, &QPushButton::clicked, this, [this] {
        startGrid(true);
    });
    connect(pauseButton, &QPushButton::clicked, this, &Window::pauseGrid);
    connect(cancelButton, &QPushButton::clicked, this, &Window::cancelBotOrders);
}

void Window::setupSummary(QVBoxLayout* layout)
{
    auto* statusRow = new QHBoxLayout;

    summaryLabel = new QLabel;
    summaryLabel->setObjectName("gridSummary");
    summaryLabel->setWordWrap(true);
    statusRow->addWidget(summaryLabel, 1);

    statsLabel = new QLabel;
    statsLabel->setObjectName("sessionStats");
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLabel->setToolTip(
        "P&L is gross, using grid entry/take prices, excluding fees and funding. Includes missed "
        "fills recovered on connection. Fees are actual USDC fees for both entries and takes; "
        "negative fees are rebates. Incomplete means some fees are unavailable. Each fully "
        "executed take order counts once. Counters reset when this window closes or Reset stats is "
        "clicked.");
    statusRow->addWidget(statsLabel);
    layout->addLayout(statusRow);
}

void Window::setupTablesAndTabs(QVBoxLayout* layout)
{
    auto* tabs = new QTabWidget;

    cyclesTable = new QTableWidget;
    setupTable(cyclesTable, {"Cycle", "Side", "Generation", "Entry", "Take", "Capacity (coin)",
                                "Open (coin)", "Reserved USDC", "Mode"});

    ordersTable = new QTableWidget;
    setupTable(ordersTable, {"Cycle / Owner", "Role / Side", "Price", "Total (coin)",
                                "Filled (coin)", "State", "Exchange ID"});

    previewTable = new QTableWidget;
    setupTable(previewTable,
        {"Side / Level", "Entry", "Take", "Quantity (coin)", "Notional (USDC)", "Status"});
    tabs->addTab(cyclesTable, "Cycles");
    tabs->addTab(ordersTable, "Orders");
    tabs->addTab(previewTable, "Current grid / Preview");
    layout->addWidget(tabs, 1);
}

void Window::setupHistoryAndJournal(QVBoxLayout* layout)
{
    auto* historyButtons = new QHBoxLayout;

    auto* clearLogButton = new QPushButton("Clear log");
    clearLogButton->setObjectName("clearLog");

    resetButton = new QPushButton("Reset after manual trades");
    resetButton->setObjectName("resetHistory");
    resetButton->setToolTip(
        "Cancel all bot orders first. Clears saved cycles/orders and tracked inventory, keeping "
        "settings and session counters. Exchange position stays open.");
    historyButtons->addWidget(clearLogButton);
    historyButtons->addWidget(resetButton);
    historyButtons->addStretch();
    layout->addLayout(historyButtons);
    connect(resetButton, &QPushButton::clicked, this, &Window::resetHistory);

    journal = new QPlainTextEdit;
    journal->setReadOnly(true);
    journal->setMaximumBlockCount(1000);
    journal->setMaximumHeight(155);

    layout->addWidget(journal);
    connect(clearLogButton, &QPushButton::clicked, journal, &QPlainTextEdit::clear);
}

void Window::setupFooter(QVBoxLayout* layout)
{
    auto* footer = new QHBoxLayout;

    latencyLabel = new QLabel;
    latencyLabel->setObjectName("connectionLatency");
    latencyLabel->setToolTip(
        "Exchange response time in milliseconds. Red means disconnected, a response or connection "
        "taking at least 2 seconds, or no incoming data for 15 seconds.");
    footer->addWidget(latencyLabel);
    footer->addStretch();

    deleteProfileButton = new QPushButton("Delete current profile");
    deleteProfileButton->setObjectName("deleteProfile");
    auto deletePalette = deleteProfileButton->palette();
    deletePalette.setColor(QPalette::ButtonText, QColor("#b91c1c"));
    deletePalette.setColor(QPalette::Button, QColor("#fee2e2"));
    deleteProfileButton->setPalette(deletePalette);
    deleteProfileButton->setToolTip("Cancel this profile's exchange orders, then clear only its "
                                    "saved settings and history. Exchange positions remain open.");
    footer->addWidget(deleteProfileButton);
    layout->addLayout(footer);
    connect(deleteProfileButton, &QPushButton::clicked, this, [this] {
        cancelProfileOrders(true);
    });
}

Config Window::uiConfig() const
{
    Config c;
    c.instrument = instrumentBox->currentText().toStdString();
    c.production = environmentBox->currentIndex() == 1;
    for (int row = 0; row < 2; ++row) {
        auto& b = row == 0 ? c.longs : c.shorts;
        b.low = fixed(qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 1))->value());
        b.high = fixed(qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 2))->value());
        b.budget = static_cast<Int>(std::llround(
            qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 3))->value() * Dollars));
        b.levels = qobject_cast<QSpinBox*>(settings->cellWidget(row, 4))->value();
    }
    const auto error = Engine::validate(c);
    if (!error.empty())
        throw std::runtime_error(error);
    return c;
}

void Window::setConfig(const Config& c)
{
    instrumentBox->setCurrentText(text(c.instrument));
    environmentBox->setCurrentIndex(c.production ? 1 : 0);
    environmentBox->setStyleSheet(
        environmentBox->currentIndex() ? "color:#b91c1c;font-weight:bold" : "");
    for (int row = 0; row < 2; ++row) {
        const auto& b = row == 0 ? c.longs : c.shorts;
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 1))->setValue(price(b.low));
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 2))->setValue(price(b.high));
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 3))
            ->setValue(static_cast<double>(b.budget) / Dollars);
        qobject_cast<QSpinBox*>(settings->cellWidget(row, 4))->setValue(b.levels);
    }
}
