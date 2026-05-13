#include "runtime/bn_aliases.h"

#include "bnl/interpreter.h"
#include "runtime/environment.h"

namespace bnl::bn_aliases {

void define_global(Interpreter& interp, std::string_view canonical, Value v) {
    auto& env = *interp.globals();

    env.define(std::string(canonical), v);

    // Bind every Bangla synonym to the same Value. Linear scan is fine —
    // the global table is small (~10 entries) and define_global is called
    // a fixed number of times at interpreter startup.
    for (const auto& alias : globals()) {
        if (alias.canonical == canonical) {
            for (const auto& bn : alias.bangla) {
                env.define(std::string(bn), v);
            }
            return;
        }
    }
    // No table entry — only the canonical name was bound. That's fine for
    // globals you haven't decided on a Bangla translation for yet.
}

}  // namespace bnl::bn_aliases
