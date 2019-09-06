#pragma once

#include "seastarx.h"

#include <seastar/core/print.hh>

#include <optional>
#include <ostream>

namespace std {

template<typename T>
std::ostream& operator<<(std::ostream& os, const std::optional<T>& opt) {
    if (opt) {
        return fmt_print(os, "{{{}}}", *opt);
    }
    return os << "{}";
}

} // namespace std