#ifdef GRID_BASELINE
#include "grid.h"
#else
#include "engine/grid.h"
#endif
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

static size_t allocations = 0;

void* operator new(size_t size)
{
    ++allocations;
    if (void* value = std::malloc(size ? size : 1))
        return value;
    throw std::bad_alloc();
}

void operator delete(void* value) noexcept
{
    std::free(value);
}

void operator delete(void* value, size_t) noexcept
{
    std::free(value);
}

using namespace grid;

template <class F> void measure(const char* name, int count, F operation)
{
    const auto before = allocations;
    const auto start = std::chrono::steady_clock::now();
    Int result = 0;
    for (int i = 0; i < count; ++i)
        result += operation();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start)
                        .count();
    std::cout << name << ": " << us << " us / " << count << ", allocations=" << allocations - before
              << ", checksum=" << result << '\n';
}

int main()
{
    Engine e;
    e.profileId = "benchmark";
    e.spec = {Scale / 10, Scale / 10000, {}};
    e.config.shorts = {80000 * Scale, 90000 * Scale, 850 * Dollars, 14};
    e.quote(82500 * Scale, 82500 * Scale);
    for (int i = 0; i < 5000; ++i) {
        Cycle c{e.nextCycle++, 0, Side::Short, 82500 * Scale, 82400 * Scale, 1, 0, true};
        Order o;
        o.label = "dg_benchmark_" + std::to_string(c.id) + "_e_" + std::to_string(e.nextOrder++);
        o.cycle = c.id;
        o.price = c.entry;
        o.total = 1;
        o.status = Status::Terminal;
#ifdef GRID_BASELINE
        e.cycles.push_back(c);
        e.orders.push_back(o);
#else
        e.importCycle(c);
        e.importOrder(o);
#endif
    }
    measure("aggregates", 100, [&] {
        return e.used(Side::Short) + e.occupiedSlots(Side::Short);
    });
    const std::string label = "dg_benchmark_5000_e_5000";
    measure("label lookup", 10000, [&] {
        return e.order(label)->total;
    });
    measure("preview", 20, [&] {
        return Int(e.preview(e.config).lattice.size());
    });
    const auto current = *e.order(label);
    Update update{label, "bench-exchange", 1, 0, 1, current.price, true};
    e.apply(update);
    measure("order update", 10000, [&] {
        ++update.time;
        e.apply(update);
        return Int(1);
    });
    measure("lot arithmetic", 10000, [&] {
        return e.lotsFor(8250000, 82500 * Scale);
    });
}
