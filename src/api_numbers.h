#pragma once
#include "grid.h"
#include <QJsonValue>
#include <cmath>
#include <stdexcept>

namespace deribit {
inline grid::Int fixed(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1000000000.0) throw std::runtime_error("Invalid API number");
    return static_cast<grid::Int>(std::llround(value * grid::Scale));
}
inline grid::Int signedFixed(double value) { return value < 0 ? -fixed(-value) : fixed(value); }
inline grid::Int apiLots(const QJsonValue& value, const grid::Spec& spec) {
    if (!value.isDouble() || spec.quantityStep <= 0) throw std::runtime_error("Missing API quantity/specification");
    const auto units = fixed(value.toDouble());
    if (units % spec.quantityStep) throw std::runtime_error("API quantity is not a multiple of the quantity step");
    return units / spec.quantityStep;
}
}
