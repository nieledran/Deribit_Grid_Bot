#pragma once
#include "grid.h"
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

QJsonObject encodeEngine(const grid::Engine& engine);
grid::Engine decodeEngine(const QJsonObject& object);
class Store {
public:
    explicit Store(const QString& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    bool load(grid::Engine& engine);
    void save(const grid::Engine& engine);
    void bindAccount(const QString& identity);
private:
    QSqlDatabase db;
    QString connection;
    QByteArray lastSaved;
};
