#pragma once
#include "firefly/actions/GenericAction.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <cerrno>
#include <cstdlib>

namespace firefly::actions {
// JSON parsers may silently turn a subnormal/underflowing token into zero.
// Inspect number lexemes before parsing (strings, including escapes, are skipped).
inline void checkNumericLexemes(std::string_view raw) {
    bool quoted = false, escaped = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c != '-' && (c < '0' || c > '9')) continue;
        const auto start = i;
        while (i < raw.size() && std::string_view("0123456789+-.eE").find(raw[i]) != std::string_view::npos) ++i;
        const std::string token(raw.substr(start,i-start));
        if (token.find_first_of(".eE") != std::string::npos) {
            std::size_t significant = 0;bool nonzero = false;
            for (char digit : token.substr(0,token.find_first_of("eE"))) if (digit >= '0' && digit <= '9') {
                nonzero = nonzero || digit != '0';if (nonzero) ++significant;
            }
            if (significant > 15) throw std::invalid_argument("Number exceeds the supported 15 significant decimal digits");
            errno = 0; char* end = nullptr;const auto value = std::strtod(token.c_str(),&end);
            if (errno == ERANGE || !std::isfinite(value)) throw std::invalid_argument("Floating-point overflow or underflow");
        }
        if (i) --i;
    }
}
inline Scalar decodeScalar(const nlohmann::json& value) {
    if (value.is_boolean()) return Scalar(value.get<bool>());
    if (value.is_number_integer()) {
        if (value.is_number_unsigned() && value.get<std::uint64_t>() >
            std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("Integer overflow");
        return Scalar(value.get<std::int64_t>());
    }
    if (value.is_number_float() && std::isfinite(value.get<double>())) return Scalar(value.get<double>());
    if (value.is_string()) return Scalar(value.get<std::string>());
    throw std::invalid_argument("Finite scalar required");
}
inline nlohmann::json scalarJson(const Scalar& value) {
    return std::visit([](const auto& item) { return nlohmann::json(item); }, value.data());
}
inline Scalar decodeTypedScalar(const nlohmann::json& value, const std::string& type) {
    auto scalar = decodeScalar(value);
    if (type == "number" && std::holds_alternative<std::int64_t>(scalar.data())) {
        const auto number = scalar.value();
        if (number < -9007199254740991LL || number > 9007199254740991LL) throw std::invalid_argument("Inexact integer-to-number representation");
        return Scalar(static_cast<double>(number));
    }
    return scalar;
}
}
