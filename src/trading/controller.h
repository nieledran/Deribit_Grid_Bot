#pragma once
#include "exchange/client.h"
#include "storage/store.h"
#include <QJsonArray>

// Owns the durable request boundary; the window only presents outcomes.
class TradeController {
public:
    struct Receipt {
        QString exchangeId;
        grid::Int delta = 0, spread = 0, previousFilled = 0;
        bool take = false, closed = false;
    };

    struct Notification {
        QJsonObject json;
        std::string label;
        bool own = false;
    };

    TradeController(grid::Engine& engine, Store& store, Client& client)
        : engine(engine), store(store), client(client)
    {
    }

    void execute(const grid::Action& action, Client::Callback completion);
    std::optional<Receipt> receiveOwnOrder(const QJsonObject& order, bool resolve,
        bool persist = true, const std::string* label = nullptr);
    std::vector<Notification> orderedNotifications(const QJsonArray& orders);

private:
    grid::Engine& engine;
    Store& store;
    Client& client;
    static RpcResult verifiedResponse(const grid::Action& action, const RpcResult& response);
};
