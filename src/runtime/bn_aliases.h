#pragma once

// Single source of truth for Bangla aliases — both for module names
// (`import "অনুরোধ"` resolves to `request`) and global identifiers
// (`ভবিষ্যৎ` is the same callable as `Future`).
//
// To add a new alias, edit the relevant table here. Keyword aliases
// (e.g. `যদি` for `if`) intentionally live in the lexer instead because
// the parser must see them as tokens at tokenization time.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bnl/value.h"

namespace bnl {
class Interpreter;  // forward decl — define_global needs it
}

namespace bnl::bn_aliases {

// ----------------------------------------------------------------------------
// Module aliases (Bangla → canonical English name).
//
// Consumed by ModuleLoader before native-module / embedded-stdlib lookup.
// A bare-name import that matches a key here resolves identically to the
// corresponding English-named module — same Module instance, same exports.
// ----------------------------------------------------------------------------

inline const std::unordered_map<std::string, std::string>& modules() {
    static const std::unordered_map<std::string, std::string> table = {
        // ---- native ----------------------------------------------------
        {"_\xe0\xa6\xab\xe0\xa6\xbe\xe0\xa6\x87\xe0\xa6\xb2",                                                              "_io"},       // _ফাইল
        {"\xe0\xa6\x9f\xe0\xa6\xbe\xe0\xa6\x87\xe0\xa6\xae\xe0\xa6\xbe\xe0\xa6\xb0",                                       "timers"},    // টাইমার
        {"\xe0\xa6\xa8\xe0\xa7\x87\xe0\xa6\x9f",                                                                           "net"},       // নেট
        {"\xe0\xa6\x9c\xe0\xa7\x87\xe0\xa6\xb8\xe0\xa6\xa8",                                                               "json"},      // জেসন
        {"\xe0\xa6\xb8\xe0\xa6\xbf\xe0\xa6\xb8\xe0\xa7\x8d\xe0\xa6\x9f\xe0\xa7\x87\xe0\xa6\xae",                           "sys"},       // সিস্টেম
        {"\xe0\xa6\xb8\xe0\xa6\xae\xe0\xa6\xaf\xe0\xa6\xbc",                                                               "time"},      // সময়
        {"\xe0\xa6\xaa\xe0\xa6\xa5",                                                                                       "path"},      // পথ
        {"\xe0\xa6\x97\xe0\xa6\xa3\xe0\xa6\xbf\xe0\xa6\xa4",                                                               "math"},      // গণিত
        {"\xe0\xa6\x8f\xe0\xa6\xb2\xe0\xa7\x8b\xe0\xa6\xae\xe0\xa7\x87\xe0\xa6\xb2\xe0\xa7\x8b",                           "random"},    // এলোমেলো
        {"\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa6\xbf\xe0\xa6\xaa\xe0\xa7\x8d\xe0\xa6\x9f\xe0\xa7\x8b",               "crypto"},    // ক্রিপ্টো
        {"\xe0\xa6\xaa\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa6\xbe\xe0\xa6\x9f\xe0\xa6\xbe\xe0\xa6\xb0\xe0\xa7\x8d\xe0\xa6\xa8",   "regex"},     // প্যাটার্ন
        {"\xe0\xa6\x8f\xe0\xa6\x87\xe0\xa6\x9a\xe0\xa6\x9f\xe0\xa6\xbf\xe0\xa6\x9f\xe0\xa6\xbf\xe0\xa6\xaa\xe0\xa6\xbf",   "http"},      // এইচটিটিপি
        {"\xe0\xa6\x9f\xe0\xa6\xbf\xe0\xa6\x8f\xe0\xa6\xb2\xe0\xa6\x8f\xe0\xa6\xb8",                                       "tls"},       // টিএলএস

        // ---- lib (embedded) --------------------------------------------
        {"\xe0\xa6\xab\xe0\xa6\xbe\xe0\xa6\x87\xe0\xa6\xb2",                                                               "io"},        // ফাইল  (public io lib facade)
        {"\xe0\xa6\x85\xe0\xa6\xa8\xe0\xa7\x81\xe0\xa6\xb0\xe0\xa7\x8b\xe0\xa6\xa7",                                       "request"},   // অনুরোধ
        {"\xe0\xa6\x93\xe0\xa6\xaf\xe0\xa6\xbc\xe0\xa7\x87\xe0\xa6\xac",                                                   "web"},       // ওয়েব
        {"\xe0\xa6\x87\xe0\xa6\x89\xe0\xa6\x86\xe0\xa6\xb0\xe0\xa6\xb2",                                                   "url"},       // ইউআরএল
        {"\xe0\xa6\xb2\xe0\xa6\x97",                                                                                       "log"},       // লগ
        {"\xe0\xa6\x9f\xe0\xa7\x87\xe0\xa6\xb8\xe0\xa7\x8d\xe0\xa6\x9f",                                                   "test"},      // টেস্ট
        {"\xe0\xa6\xb8\xe0\xa6\x82\xe0\xa6\x95\xe0\xa7\x8b\xe0\xa6\x9a\xe0\xa6\xa8",                                       "zlib"},      // সংকোচন
        {"\xe0\xa6\x8f\xe0\xa6\xb8\xe0\xa6\x95\xe0\xa6\xbf\xe0\xa6\x89\xe0\xa6\xb2\xe0\xa6\xbe\xe0\xa6\x87\xe0\xa6\x9f",   "sqlite"},    // এসকিউলাইট
        {"\xe0\xa6\xaa\xe0\xa7\x8b\xe0\xa6\xb8\xe0\xa7\x8d\xe0\xa6\x9f\xe0\xa6\x97\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa7\x87\xe0\xa6\xb8", "pg"},      // পোস্টগ্রেস
        {"\xe0\xa6\xae\xe0\xa6\x99\xe0\xa7\x8d\xe0\xa6\x97\xe0\xa7\x8b",                                                   "mongo"},     // মঙ্গো
        {"\xe0\xa6\x9a\xe0\xa6\xbe\xe0\xa6\xb2\xe0\xa6\xbe\xe0\xa6\xa8",                                                   "exec"},      // চালান
        {"\xe0\xa6\xa1\xe0\xa6\xbf\xe0\xa6\x8f\xe0\xa6\xa8\xe0\xa6\x8f\xe0\xa6\xb8",                                       "dns"},       // ডিএনএস
        {"\xe0\xa6\x9f\xe0\xa7\x87\xe0\xa6\xae\xe0\xa6\xaa\xe0\xa7\x8d\xe0\xa6\xb2\xe0\xa7\x87\xe0\xa6\x9f",               "template"},  // টেমপ্লেট
        {"\xe0\xa6\x93\xe0\xa6\xaf\xe0\xa6\xbc\xe0\xa7\x87\xe0\xa6\xac\xe0\xa6\xb8\xe0\xa6\x95\xe0\xa7\x87\xe0\xa6\x9f",   "ws"},        // ওয়েবসকেট
        {"\xe0\xa6\x95\xe0\xa7\x81\xe0\xa6\x95\xe0\xa6\xbf",                                                               "cookie"},    // কুকি
        {"\xe0\xa6\x85\xe0\xa6\xa7\xe0\xa6\xbf\xe0\xa6\xac\xe0\xa7\x87\xe0\xa6\xb6\xe0\xa6\xa8",                           "session"},   // অধিবেশন
        {"\xe0\xa6\xae\xe0\xa6\xbe\xe0\xa6\xb2\xe0\xa7\x8d\xe0\xa6\x9f\xe0\xa6\xbf\xe0\xa6\xaa\xe0\xa6\xbe\xe0\xa6\xb0\xe0\xa7\x8d\xe0\xa6\x9f", "multipart"}, // মাল্টিপার্ট
        {"\xe0\xa6\xa1\xe0\xa6\x9f\xe0\xa6\x8f\xe0\xa6\xa8\xe0\xa6\xad",                                                   "dotenv"},    // ডটএনভ
        {"\xe0\xa6\x95\xe0\xa6\xae\xe0\xa6\xbe\xe0\xa6\xa8\xe0\xa7\x8d\xe0\xa6\xa1",                                       "cli"},       // কমান্ড
        {"\xe0\xa6\x87\xe0\xa6\x89\xe0\xa6\x87\xe0\xa6\x89\xe0\xa6\x86\xe0\xa6\x87\xe0\xa6\xa1\xe0\xa6\xbf",               "uuid"},      // ইউইউআইডি
        {"\xe0\xa6\xb8\xe0\xa6\xbf\xe0\xa6\x8f\xe0\xa6\xb8\xe0\xa6\xad\xe0\xa6\xbf",                                       "csv"},       // সিএসভি
    };
    return table;
}

// Look up `name` in the module alias table; return the canonical English
// name if matched, otherwise return `name` unchanged.
inline const std::string& canonical_module(const std::string& name) {
    const auto& table = modules();
    auto it = table.find(name);
    return it == table.end() ? name : it->second;
}

// ----------------------------------------------------------------------------
// Global identifier aliases.
//
// Consumed by register_builtins / register_future / etc. via define_global —
// the same Value gets bound under every name listed for its canonical entry.
// Globals without a chosen Bangla translation simply have an empty vector.
// ----------------------------------------------------------------------------

struct GlobalAlias {
    std::string_view              canonical;   // English name
    std::vector<std::string_view> bangla;      // Bangla synonyms (UTF-8)
};

inline const std::vector<GlobalAlias>& globals() {
    static const std::vector<GlobalAlias> v = {
        {"print",    {"\xe0\xa6\xb2\xe0\xa6\xbf\xe0\xa6\x96\xe0\xa7\x81\xe0\xa6\xa8"}},                                                                                                                          // লিখুন
        {"type",     {"\xe0\xa6\xa7\xe0\xa6\xb0\xe0\xa6\xa3"}},                                                                                                                                                  // ধরণ
        {"input",    {"\xe0\xa6\x87\xe0\xa6\xa8\xe0\xa6\xaa\xe0\xa7\x81\xe0\xa6\x9f"}},                                                                                                                          // ইনপুট
        {"Future",   {"\xe0\xa6\xad\xe0\xa6\xac\xe0\xa6\xbf\xe0\xa6\xb7\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa7\x8e"}},                                                                                                  // ভবিষ্যৎ
        {"futurify", {"\xe0\xa6\xad\xe0\xa6\xac\xe0\xa6\xbf\xe0\xa6\xb7\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa7\x8e\xe0\xa6\x95\xe0\xa6\xb0"}},                                                                          // ভবিষ্যৎকর
        {"str",       {"\xe0\xa6\xb8\xe0\xa7\x8d\xe0\xa6\x9f\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa6\xbf\xe0\xa6\x82"}},                                                  // স্ট্রিং
        {"to_number", {"\xe0\xa6\xb8\xe0\xa6\x82\xe0\xa6\x96\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa6\xbe"}},                                                              // সংখ্যা
        {"chr",       {"\xe0\xa6\x85\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb7\xe0\xa6\xb0"}},                                                                          // অক্ষর
        {"try_call",  {"\xe0\xa6\xa8\xe0\xa6\xbf\xe0\xa6\xb0\xe0\xa6\xbe\xe0\xa6\xaa\xe0\xa6\xa6_\xe0\xa6\x95\xe0\xa6\xb2"}},                                      // নিরাপদ_কল
        {"pretty",    {"\xe0\xa6\xb8\xe0\xa7\x81\xe0\xa6\xa8\xe0\xa7\x8d\xe0\xa6\xa6\xe0\xa6\xb0"}},                                                              // সুন্দর
        {"dump",      {"\xe0\xa6\xac\xe0\xa6\xbf\xe0\xa6\xb8\xe0\xa7\x8d\xe0\xa6\xa4\xe0\xa6\xbe\xe0\xa6\xb0\xe0\xa6\xbf\xe0\xa6\xa4"}},                          // বিস্তারিত
    };
    return v;
}

// Bind `v` in the interpreter's globals under `canonical` and every Bangla
// synonym listed for it in the table. Calls falling through with an unknown
// canonical name simply bind that one name (so adding a new global doesn't
// require updating the table first).
void define_global(Interpreter& interp, std::string_view canonical, Value v);

}  // namespace bnl::bn_aliases
