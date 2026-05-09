#pragma once

#include <string>
#include <vector>

#include "ast.h"

namespace bnl {

std::string ast_to_string(const std::vector<StmtPtr>& stmts);

}  // namespace bnl
