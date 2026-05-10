#pragma once

#include <string>
#include <vector>

#include "bnl/ast.h"

namespace bnl {

std::string ast_to_string(const std::vector<StmtPtr>& stmts);

}  // namespace bnl
