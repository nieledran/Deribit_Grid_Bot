#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QWebSocket>
#include <iostream>

// This probe is public and read-only. It never authenticates or submits orders.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QWebSocket socket;
    int received = 0;
    QObject::connect(&socket, &QWebSocket::connected, &app, [&] {
        int id = 0;
        for (const QString& instrument :
            {QString("BTC_USDC-PERPETUAL"), QString("ETH_USDC-PERPETUAL")}) {
            const QJsonObject message{{"jsonrpc", "2.0"}, {"id", ++id},
                {"method", "public/get_instrument"},
                {"params", QJsonObject{{"instrument_name", instrument}}}};
            socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson()));
        }
    });
    QObject::connect(&socket, &QWebSocket::textMessageReceived, &app, [&](const QString& message) {
        const auto root = QJsonDocument::fromJson(message.toUtf8()).object();
        if (root.contains("error")) {
            std::cerr << message.toStdString() << '\n';
            app.exit(1);
            return;
        }
        const auto o = root["result"].toObject();
        if (o["settlement_currency"].toString() != "USDC" || o["kind"].toString() != "future") {
            app.exit(1);
            return;
        }
        std::cout << o["instrument_name"].toString().toStdString()
                  << " state=" << o["state"].toString().toStdString()
                  << " tick=" << o["tick_size"].toDouble()
                  << " quantity_step=" << o["min_trade_amount"].toDouble()
                  << " contract_size=" << o["contract_size"].toDouble() << '\n';
        if (++received == 2)
            app.exit(0);
    });
    QObject::connect(&socket, &QWebSocket::errorOccurred, &app, [&](QAbstractSocket::SocketError) {
        std::cerr << socket.errorString().toStdString() << '\n';
        app.exit(1);
    });
    QTimer::singleShot(20000, &app, [&] {
        std::cerr << "Testnet probe timed out\n";
        app.exit(1);
    });
    socket.open(QUrl("wss://test.deribit.com/ws/api/v2"));
    return app.exec();
}
