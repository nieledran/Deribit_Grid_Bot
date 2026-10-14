#include "ui/window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QMessageBox>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("DeribitGrid");
    app.setApplicationVersion("0.1.0");
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#ffffff"));
    palette.setColor(QPalette::WindowText, Qt::black);
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor("#f4f5f6"));
    palette.setColor(QPalette::Text, Qt::black);
    palette.setColor(QPalette::Button, QColor("#f1f2f3"));
    palette.setColor(QPalette::ButtonText, Qt::black);
    palette.setColor(QPalette::PlaceholderText, QColor("#6b7280"));
    palette.setColor(QPalette::Highlight, QColor("#dbeafe"));
    palette.setColor(QPalette::HighlightedText, Qt::black);
    app.setPalette(palette);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"profile", "SQLite profile path (optional)", "path"});
    parser.addOption({"instrument", "Instrument for a new profile", "name", "BTC_USDC-PERPETUAL"});
    parser.addOption({"production", "Use Production for a new profile"});
    parser.addOption({"smoke-test", "Start the UI without connecting and exit after one second"});
    parser.addOption({"screenshot", "Save an offline UI screenshot", "path"});
    parser.process(app);
    try {
        QString instrument = parser.value("instrument");
        QString profile = parser.value("profile");
        bool production = parser.isSet("production");
        if ((profile.isEmpty() || parser.isSet("smoke-test")) && !parser.isSet("instrument") &&
            !parser.isSet("production")) {
            QDialog selection;
            selection.setWindowTitle("Deribit Grid — Settings");
            selection.setMinimumWidth(420);
            auto* form = new QFormLayout(&selection);
            auto* instruments = new QComboBox(&selection);
            instruments->addItems({"BTC_USDC-PERPETUAL", "ETH_USDC-PERPETUAL"});
            auto* environments = new QComboBox(&selection);
            environments->addItems({"Testnet", "Production"});
            form->addRow("Instrument:", instruments);
            form->addRow("Environment:", environments);
            auto* note = new QLabel("Each instrument and environment has its own saved "
                                    "settings.\nTo run both instruments, launch the program again.",
                &selection);
            note->setWordWrap(true);
            form->addRow(note);
            auto* buttons =
                new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &selection);
            buttons->button(QDialogButtonBox::Ok)->setText("Open");
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &selection, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
            if (parser.isSet("screenshot"))
                QTimer::singleShot(150, &selection, [&] {
                    selection.grab().save(parser.value("screenshot") + ".settings.png");
                });
            if (parser.isSet("smoke-test"))
                QTimer::singleShot(250, &selection, &QDialog::accept);
            if (selection.exec() != QDialog::Accepted)
                return 0;
            instrument = instruments->currentText();
            production = environments->currentIndex() == 1;
        }
        if (profile.isEmpty()) {
            const QString base = instrument.startsWith("ETH") ? "eth" : "btc";
            profile = QCoreApplication::applicationDirPath() + "/profiles/" + base +
                      (production ? "-prod.db" : "-test.db");
        }
        Window window(QFileInfo(profile).absoluteFilePath(), instrument, nullptr, {}, production);
        window.show();
        if (parser.isSet("screenshot"))
            QTimer::singleShot(500, &window, [&] {
                window.grab().save(parser.value("screenshot"));
            });
        if (parser.isSet("smoke-test"))
            QTimer::singleShot(1000, &window, &QWidget::close);
        return app.exec();
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Cannot open profile", QString::fromUtf8(e.what()));
        return 1;
    }
}
