#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

struct TSParser;
struct TSTree;

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

class GDScriptTreeSitter : public RefCounted {
    GDCLASS(GDScriptTreeSitter, RefCounted);

protected:
    TSParser  *_parser  = nullptr;
    TSTree    *_tree    = nullptr;
    CharString _src;
    uint32_t   _src_len = 0;
    bool       _edited  = false;

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
};

} // namespace godot
