#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <functional>
#include <map>
#include <deque>

struct RpcResult {
    bool ok = false;
    QJsonValue value;
    QString error;
    bool uncertain = false;
};
class Client : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const RpcResult&)>;
    explicit Client(QObject* parent = nullptr);
    void connectTo(const QString& id, const QString& secret, bool production, const QUrl& testEndpoint = {});
    void stop();
    qint64 call(const QString& method, const QJsonObject& params, Callback callback, bool mutation = false);
    bool authenticated() const { return authorized; }
signals:
    void authenticatedSession(const QJsonObject& auth);
    void lost();
    void notice(const QString& text);
    void subscription(const QString& channel, const QJsonValue& data);
private:
    struct Pending { Callback callback; qint64 deadline; bool mutation; };
    QWebSocket socket;
    QTimer retry, refresh, watchdog, sendTimer;
    QElapsedTimer clock;
    std::map<qint64, Pending> pending;
    std::deque<std::pair<qint64, QJsonObject>> outbound;
    QString clientId, clientSecret, refreshToken;
    QUrl endpoint;
    qint64 sequence = 0, lastMessage = 0, lastSent = -100;
    int retryMs = 1500;
    bool wanted = false, authorized = false;
    void authenticate(bool renew);
    void receive(const QString& message);
    void disconnected();
    void sendNext();
};
