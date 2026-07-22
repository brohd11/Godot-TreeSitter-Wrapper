#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <tree_sitter/api.h>
#include <cstdint>
#include <utility>
#include <vector>

namespace godot {

// Base tree-sitter binding for Godot.
//
// Handles the parser/tree lifecycle and exposes:
//   open_text / open_file     — full parse
//   update_text               — set the new full text; diffs against the previous
//                               source in C++ and incrementally reparses in one call
//                               (replaces the apply_edit + reparse_text dance)
//   reparse_text              — incremental reparse (pass after apply_edit)
//   apply_edit                — hint the byte range that changed (UTF-8 byte offsets;
//                               CodeEdit gives char columns — identical for ASCII,
//                               diverge on multibyte characters)
//   query(pattern)            — run a tree-sitter S-expression query; returns an
//                               Array of Dictionaries, one per match:
//                               { capture_name: { "text", "start_line", "end_line",
//                                                 "start_col", "end_col" } }
//
// All query methods return empty values if called between apply_edit and reparse_text.
//
// Bracket mode (set_bracket_mode(true)) maintains a per-line map of bracket
// tokens (()[]{}) for colored-bracket highlighting, updated incrementally on
// every parse: update_text() re-scans only the rows touched by the edit (byte
// diff union tree-sitter changed ranges, so structural fall-out like an
// unmatched quote restringing the lines below is covered) and syncs the
// public Dictionary in place — the cost is paid at parse time, not read time.
// A wholesale text replacement (e.g. swapping scripts on a shared instance)
// triggers a full rescan instead, so no rows from the previous document can
// survive; clear_brackets() drops the map without disabling bracket mode.
// get_brackets() returns that Dictionary by reference: { line: { column:
// depth } } — all ints, insertion-ordered, lines with no brackets absent;
// column is a *character* column (unlike the byte columns of query()/
// apply_edit()); depth is the raw running nesting level, shared by an opener
// and its match. Depth is NOT clamped at 0: a stray closer pushes it negative
// until balanced — color with posmod(depth, n). Brackets inside strings/
// comments are not tokens and never appear.
//
// The returned Dictionary is always the same object (full scans clear()+refill
// it), so callers may fetch it once and hold the reference across edits.
// Treat it as read-only: mutating it corrupts the maintenance bookkeeping.

class GDScriptTreeSitter : public RefCounted {
    GDCLASS(GDScriptTreeSitter, RefCounted);

protected:
    TSParser  *_parser  = nullptr;
    TSTree    *_tree    = nullptr;
    CharString _src;
    uint32_t   _src_len = 0;
    bool       _edited  = false;

    // --- bracket mode state -------------------------------------------------
    struct BracketLine {
        std::vector<std::pair<int32_t, int32_t>> entries; // (char column, depth), source order
        int32_t end_depth = 0;                            // running depth after this line
    };
    bool                    _bracket_mode = false;
    std::vector<BracketLine> _bracket_lines; // one per row, including empty rows
    std::vector<uint32_t>    _line_starts;   // byte offset of each row's first byte
    // Public view derived from _bracket_lines, kept up-to-date by every parse.
    // Always the same object: full scans clear()+refill it so GDScript can hold
    // the reference across edits. Read-only for consumers.
    Dictionary             _brackets_dict;

    void _rebuild_line_starts();
    // Fills _bracket_lines rows [from_row, to_row] by walking `scope` (must
    // cover those rows), with `start_depth` = running depth at from_row.
    void _scan_brackets(TSNode scope, int32_t start_depth, uint32_t from_row, uint32_t to_row);
    void _brackets_full_scan();
    // Incremental update after update_text(): byte-diff rows plus tree-sitter
    // changed ranges (new-tree coordinates) delimit the rows to re-scan.
    // p_new_tree is the freshly parsed tree (also stored in _tree by then);
    // the old tree stays alive until after this runs.
    void _brackets_after_edit(uint32_t start_row, uint32_t old_end_row, uint32_t new_end_row,
                              const TSRange *ranges, uint32_t range_count, TSTree *p_new_tree);

    static void _bind_methods();

public:
    GDScriptTreeSitter();
    ~GDScriptTreeSitter();

    void open_text(const String &p_text);
    bool open_file(const String &p_path);
    void apply_edit(int p_start_byte,
                    int p_old_end_byte, int p_new_end_byte,
                    int p_start_row,    int p_start_col,
                    int p_old_end_row,  int p_old_end_col,
                    int p_new_end_row,  int p_new_end_col);
    void reparse_text(const String &p_text);
    void update_text(const String &p_new_text);
    Array query(const String &p_pattern);

    void set_bracket_mode(bool p_enabled);
    bool get_bracket_mode() const;
    Dictionary get_brackets() const;
    // Drops the bracket map without disabling bracket mode — the next parse
    // rebuilds it. For script swaps / detach on a shared parse instance.
    void clear_brackets();
};

} // namespace godot
