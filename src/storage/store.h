#pragma once
#include "engine/grid.h"
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

QJsonObject encodeEngine(const grid::Engine& engine, bool records = true);
grid::Engine decodeEngine(const QJsonObject& object);

class Store {
public:
    explicit Store(const QString& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    bool load(grid::Engine& engine);
    void save(grid::Engine& engine);
    bool recoverOrder(const std::string& label, grid::Engine& engine);
    bool ownsExchangeId(const QString& id);
    void expectFee(const QString& id, grid::Int lots);
    grid::Int feeValue(const QString& id, bool covered = false);
    bool recordFee(const QString& key, const QString& id, grid::Int lots);
    void resetFees();
    void resetProfile(const grid::Engine& fresh);
    QStringList missingFees();
    bool feesComplete();
    void bindAccount(const QString& identity);

private:
    QSqlDatabase db;
    QString connection;
    QByteArray lastSaved;
    QJsonObject lastMetadata;
    const grid::Engine* lastEngine = nullptr;
    std::uint64_t lastRevision = ~std::uint64_t(0);
};
