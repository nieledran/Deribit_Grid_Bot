#ifdef GRID_BASELINE
#include "store.h"
#else
#include "storage/store.h"
#endif
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace grid;

size_t workingSet()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return counters.WorkingSetSize;
#endif
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        if (argc == 2) {
            Store store(QString::fromLocal8Bit(argv[1]));
            Engine e;
            if (!store.load(e))
                throw std::runtime_error("Empty profile");
            store.save(e);
#ifndef GRID_BASELINE
            std::cout << "Migrated snapshot: cycles=" << e.cycles().size()
                      << ", orders=" << e.orders().size() << '\n';
#endif
            return 0;
        }
        QTemporaryDir dir;
        Store store(dir.filePath("benchmark.db"));
        Engine e;
        e.profileId = "benchmark";
        e.spec = {Scale / 10, Scale / 10000, {}};
        e.config.shorts = {80000 * Scale, 90000 * Scale, 850 * Dollars, 14};
        for (int i = 0; i < 5000; ++i) {
            Cycle c{e.nextCycle++, 0, Side::Short, 82500 * Scale, 82400 * Scale, 2, 0, true};
            Order o;
            o.label =
                "dg_benchmark_" + std::to_string(c.id) + "_e_" + std::to_string(e.nextOrder++);
            o.cycle = c.id;
            o.price = c.entry;
            o.total = 2;
            o.exchangeId = "exchange-" + std::to_string(c.id);
            o.status = Status::Terminal;
#ifdef GRID_BASELINE
            e.cycles.push_back(c);
            e.orders.push_back(o);
#else
            e.importCycle(c);
            e.importOrder(o);
#endif
        }
        QElapsedTimer clock;
        clock.start();
        store.save(e);
        std::cout << "initial save/archive: " << clock.nsecsElapsed() / 1000 << " us\n";
        clock.restart();
        for (int i = 0; i < 100; ++i)
            store.save(e);
        std::cout << "idle save: " << clock.nsecsElapsed() / 1000 << " us / 100\n";
        const auto order = *e.order("dg_benchmark_5000_e_5000");
        clock.restart();
        for (int i = 0; i < 20; ++i) {
            e.apply({order.label, order.exchangeId, 2, 1, i + 1, order.price, true});
            store.save(e);
        }
        std::cout << "event + durable save: " << clock.nsecsElapsed() / 1000 << " us / 20\n";
#ifdef GRID_BASELINE
        std::cout << "retained orders: " << e.orders.size() << '\n';
#else
        std::cout << "retained orders: " << e.orders().size() << '\n';
        std::cout << "working set after 5000: " << workingSet() << " bytes\n";
        // Grow only the archive, using small batches as in a running bot.
        for (int batch = 0; batch < 400; ++batch) {
            for (int i = 0; i < 100; ++i) {
                Cycle c{e.nextCycle++, 0, Side::Short, 82500 * Scale, 82400 * Scale, 2, 0, true};
                Order o;
                o.label =
                    "dg_benchmark_" + std::to_string(c.id) + "_e_" + std::to_string(e.nextOrder++);
                o.cycle = c.id;
                o.price = c.entry;
                o.total = 2;
                o.status = Status::Terminal;
                e.importCycle(c);
                e.importOrder(o);
            }
            store.save(e);
            if (batch == 199)
                std::cout << "working set after 25000: " << workingSet() << " bytes\n";
        }
        std::cout << "working set after 45000: " << workingSet()
                  << " bytes, retained orders=" << e.orders().size() << '\n';
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
