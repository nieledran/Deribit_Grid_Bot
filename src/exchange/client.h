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
    ~Client() override;
    void connectTo(
        const QString& id, const QString& secret, bool production, const QUrl& testEndpoint = {});
    void stop();
    qint64 call(
        const QString& method, const QJsonObject& params, Callback callback, bool mutation = false);

    bool authenticated() const
    {
        return authorized;
    }

    struct ConnectionStatus {
        enum class State { Idle, Connecting, Online, Offline };
        State state = State::Idle;
        qint64 milliseconds = -1;
        bool waiting = false, slow = false;
    };

    ConnectionStatus connectionStatus() const;
    void setTradingLimits(const QJsonObject& limits);
signals:
    void authenticatedSession(const QJsonObject& auth);
    void lost();
    void notice(const QString& text);
    void subscription(const QString& channel, const QJsonValue& data);

private:
    static constexpr int MinimumTradingIntervalMs = 500; //limit on how fast to send orders.
    static constexpr int InitialRetryMs = 1500;

    struct Pending {
        Callback callback;
        qint64 deadline;
        bool mutation;
        qint64 sentAt = -1;
    };

    QWebSocket socket;
    QTimer retry, refresh, watchdog, sendTimer;
    QElapsedTimer clock;
    std::map<qint64, Pending> pending;
    std::deque<std::pair<qint64, QJsonObject>> outbound;
    QString clientId, clientSecret, refreshToken;
    QUrl endpoint;
    qint64 sequence = 0, lastMessage = 0, lastSent = -100;
    qint64 lastMutation = -MinimumTradingIntervalMs;
    qint64 lastLatency = -1, connectionStarted = 0;
    int mutationInterval = MinimumTradingIntervalMs;
    int retryMs = InitialRetryMs;
    bool wanted = false, authorized = false;
    bool connectionFailed = false;
    void authenticate(bool renew);
    void receiveAuthentication(const RpcResult& r, bool renew);
    void enableHeartbeat(const QJsonObject& auth);
    void receive(const QString& message);
    void disconnected();
    void sendNext();
};
