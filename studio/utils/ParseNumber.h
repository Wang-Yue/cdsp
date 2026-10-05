#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

// Non-throwing replacements for std::stod / std::stoi.
//
// They accept exactly what the std:: versions accept (leading whitespace, then the longest
// valid numeric prefix, as std::stod/std::stoi are specified in terms of strtod/strtol), but
// report failure via the return value instead of throwing. This matters on WebAssembly:
// Emscripten builds disable C++ exception catching by default, so a `throw` inside
// `try { ... } catch (...)` calls abort() and kills the app (e.g. parsing an AutoEq preset,
// where std::stoi("ON") was used as an "is this a number?" probe).
namespace parse {

// Parses a double from the start of `s`. Returns false (leaving `out` unchanged) if no
// conversion could be performed or the value is out of range, i.e. where std::stod throws.
inline bool toDouble(const std::string& s, double& out) {
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(begin, &end);
    if (end == begin || errno == ERANGE) {
        return false;
    }
    out = value;
    return true;
}

// Parses a base-10 int from the start of `s`. Returns false (leaving `out` unchanged) if no
// conversion could be performed or the value does not fit in an int, i.e. where std::stoi throws.
inline bool toInt(const std::string& s, int& out) {
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(begin, &end, 10);
    if (end == begin || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

} // namespace parse
