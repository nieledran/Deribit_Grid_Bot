#pragma once
#include "exchange/client.h"
#include "storage/store.h"
#include <QLockFile>
#include <QMap>
#include <memory>

class ProfileController : public QObject {
    Q_OBJECT
public:
    ProfileController(grid::Engine& engine, Store& store, Client& client, const int& session,
        std::unique_ptr<QLockFile> profileLock, QObject* parent);
    void bindAccount(const QString& identity);
    bool hasAccountLock() const;
    bool cancelling() const;
    void cancelOrders(bool eraseProfile, int epoch);
    void resetHistory(int epoch);

signals:
    void cancelled();
    void profileRecreated();
    void positionRead(double position);
    void historyReadFinished();
    void historyCleared();
    void failed(const QString& message, bool cancellation);

private:
    grid::Engine& engine;
    Store& store;
    Client& client;
    const int& session;
    std::unique_ptr<QLockFile> profileLock;
    std::unique_ptr<QLockFile> accountLock;
    bool cancellationActive = false;

    void cancelProfileLabels(QMap<QString, grid::Int> labels, bool eraseProfile);
    void receiveCancellationOrders(const RpcResult& response, int epoch, bool eraseProfile);
    QMap<QString, grid::Int> cancellationLabels(const RpcResult& response) const;
    void verifyProfileCancellation(bool eraseProfile, int checks);
    void readCancellationSnapshot(int epoch, bool eraseProfile, int checks);
    void receiveCancellationSnapshot(
        const RpcResult& response, int epoch, bool eraseProfile, int checks);
    void validateCancellationSnapshot(const RpcResult& response) const;
    void recreateProfile(int epoch);
    void receiveRecreationPosition(const RpcResult& response, int epoch);
    void finishCancellation();
    void readResetPosition(int epoch);
    void receiveHistoryOrders(const RpcResult& response, int epoch);
    void receiveResetPosition(const RpcResult& response, int epoch);
};
