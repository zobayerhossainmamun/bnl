#pragma once

#include <cstddef>
#include <string>

namespace bnl {

struct Diagnostic {
    std::size_t line;
    std::size_t column;
    std::string message;
};

}  // namespace bnl
