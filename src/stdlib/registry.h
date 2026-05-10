#pragma once

namespace bnl {

class Interpreter;

// Each registers one native module under its conventional name on
// the given interpreter (so `import "sys" as sys;` finds it).
void register_sys   (Interpreter& interp);
void register_io    (Interpreter& interp);
void register_timers(Interpreter& interp);
void register_crypto(Interpreter& interp);
void register_zlib  (Interpreter& interp);
void register_httpp (Interpreter& interp);
void register_net   (Interpreter& interp);
void register_tls   (Interpreter& interp);
void register_exec  (Interpreter& interp);
void register_json  (Interpreter& interp);

}  // namespace bnl
