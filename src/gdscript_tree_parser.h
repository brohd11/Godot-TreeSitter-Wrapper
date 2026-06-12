#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

struct TSParser;
struct TSTree;

namespace godot {

// Query-style tree-sitter binding for Godot.
//
// Typical usage:
//   var p = GDScriptTreeParser.new()
//   p.open_text(source)              # or open_file(path) → bool
//
//   p.get_class_paths()              # Array  — ["", "Inner", "Outer.Inner"]
//   p.get_extends("")                # String — parent class of root
//   p.get_members("")                # Dictionary — see below
//   p.get_constants("")              # Dictionary
//   p.get_inner_classes("")          # Dictionary
//
//   # Incremental update after a text edit:
//   p.apply_edit(...)                # mark changed byte range (see note below)
//   p.reparse_text(new_source)       # incremental reparse; queries safe again
//
// ── apply_edit column note ──────────────────────────────────────────────────
// TSInputEdit expects UTF-8 *byte* offsets for columns; CodeEdit gives *char*
// (code-point) columns. These match for ASCII but diverge on multibyte
// characters (é, CJK, emoji). If your source may contain non-ASCII, convert
// CodeEdit char columns to byte columns before calling apply_edit, or use
// open_text / reparse_text without apply_edit (still incremental, just not
// byte-precise).
//
// ── Member dict formats ─────────────────────────────────────────────────────
// get_members()      { name: { "keyword", "line", "type", "changed",
//                              # funcs also have:
//                              "end_line", "return_type",
//                              "args":   { param → { "type","default","variadic" } },
//                              "locals": { name  → { "keyword","line","type"[,"lambda"] } },
//                              # vars (class-level or local) whose value is a lambda also have:
//                              "lambda": { "args", "return_type", "line", "end_line",
//                                          "locals": { … same structure, recursive } } } }
// get_constants()    { name: { "keyword", "line", "type", "changed" } }
// get_inner_classes(){ name: { "line", "extends", "changed" } }
//
// All three query methods accept an optional changed_only: bool = false.
// When true, only entries whose "changed" field is true are returned — lets
// callers skip unchanged members after an incremental reparse.
//
// "changed" is true when the node was affected by the most recent incremental
// reparse — lets callers skip unchanged members entirely.
// Lines are 0-indexed (matching Godot CodeEdit line indices).

class GDScriptTreeParser : public RefCounted {
    GDCLASS(GDScriptTreeParser, RefCounted);

    TSParser  *_parser  = nullptr;
    TSTree    *_tree    = nullptr;
    CharString _src;             // UTF-8 source kept alive for text extraction
    uint32_t   _src_len = 0;     // cached length used as OOB clamp in ts_text
    bool       _edited  = false; // set by apply_edit, cleared by open/reparse

protected:
    static void _bind_methods();

public:
    GDScriptTreeParser();
    ~GDScriptTreeParser();

    // ---- Open / reparse ---------------------------------------------------
    void open_text(const String &p_text);
    bool open_file(const String &p_path); // returns false on path error

    // Mark a single contiguous edit before calling reparse_text.
    // All values are 0-indexed and UTF-8-byte-based (see column note above).
    void apply_edit(int p_start_byte,
                    int p_old_end_byte, int p_new_end_byte,
                    int p_start_row,    int p_start_col,
                    int p_old_end_row,  int p_old_end_col,
                    int p_new_end_row,  int p_new_end_col);

    void reparse_text(const String &p_text);

    // ---- Queries (lazily walk the stored tree) ----------------------------
    // All return empty / "" if called between apply_edit and reparse_text.
    Array      get_class_paths();
    String     get_extends(const String &p_path);
    Dictionary get_members(const String &p_path, bool p_changed_only = false);
    Dictionary get_constants(const String &p_path, bool p_changed_only = false);
    Dictionary get_inner_classes(const String &p_path, bool p_changed_only = false);
};

} // namespace godot
