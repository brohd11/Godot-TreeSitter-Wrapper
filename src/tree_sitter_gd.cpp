#include "tree_sitter_gd.h"
#include "ts_helpers.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <tree_sitter/api.h>

extern "C" const TSLanguage *tree_sitter_gdscript();

namespace godot {

GDScriptTreeSitter::GDScriptTreeSitter() {
    _parser = ts_parser_new();
    ERR_FAIL_COND_MSG(
        !ts_parser_set_language(_parser, tree_sitter_gdscript()),
        "GDScriptTreeSitter: language ABI mismatch — rebuild the GDExtension.");
}

GDScriptTreeSitter::~GDScriptTreeSitter() {
    if (_tree)   { ts_tree_delete(_tree);     _tree   = nullptr; }
    if (_parser) { ts_parser_delete(_parser); _parser = nullptr; }
}

void GDScriptTreeSitter::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open_text", "text"),    &GDScriptTreeSitter::open_text);
    ClassDB::bind_method(D_METHOD("open_file", "path"),    &GDScriptTreeSitter::open_file);
    ClassDB::bind_method(D_METHOD("reparse_text", "text"), &GDScriptTreeSitter::reparse_text);
    ClassDB::bind_method(
        D_METHOD("apply_edit",
                 "start_byte",  "old_end_byte",  "new_end_byte",
                 "start_row",   "start_col",
                 "old_end_row", "old_end_col",
                 "new_end_row", "new_end_col"),
        &GDScriptTreeSitter::apply_edit);
    ClassDB::bind_method(D_METHOD("query", "pattern"), &GDScriptTreeSitter::query);
}

void GDScriptTreeSitter::open_text(const String &p_text) {
    _src     = p_text.utf8();
    _src_len = (uint32_t)_src.length();
    _edited  = false;
    TSTree *new_tree = ts_parser_parse_string(_parser, nullptr,
                                              _src.get_data(), _src_len);
    ERR_FAIL_COND_MSG(!new_tree, "GDScriptTreeSitter: ts_parser_parse_string returned null.");
    if (_tree) ts_tree_delete(_tree);
    _tree = new_tree;
}

bool GDScriptTreeSitter::open_file(const String &p_path) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
    ERR_FAIL_COND_V_MSG(!f.is_valid(), false,
        "GDScriptTreeSitter: cannot open file: " + p_path);
    open_text(f->get_as_text());
    return true;
}

void GDScriptTreeSitter::apply_edit(int p_start_byte,
                                     int p_old_end_byte, int p_new_end_byte,
                                     int p_start_row,    int p_start_col,
                                     int p_old_end_row,  int p_old_end_col,
                                     int p_new_end_row,  int p_new_end_col) {
    if (!_tree) return;
    if (p_start_byte < 0 || p_old_end_byte < 0 || p_new_end_byte < 0 ||
        p_start_row  < 0 || p_start_col    < 0 ||
        p_old_end_row < 0 || p_old_end_col < 0 ||
        p_new_end_row < 0 || p_new_end_col < 0) return;

    TSInputEdit edit;
    edit.start_byte    = (uint32_t)p_start_byte;
    edit.old_end_byte  = (uint32_t)p_old_end_byte;
    edit.new_end_byte  = (uint32_t)p_new_end_byte;
    edit.start_point   = { (uint32_t)p_start_row,    (uint32_t)p_start_col   };
    edit.old_end_point = { (uint32_t)p_old_end_row,  (uint32_t)p_old_end_col };
    edit.new_end_point = { (uint32_t)p_new_end_row,  (uint32_t)p_new_end_col };
    ts_tree_edit(_tree, &edit);
    _edited = true;
}

void GDScriptTreeSitter::reparse_text(const String &p_text) {
    _src     = p_text.utf8();
    _src_len = (uint32_t)_src.length();
    bool was_edited = _edited;
    _edited  = false;
    // Only reuse old tree nodes when apply_edit was called first; otherwise the
    // old positions are in the wrong coordinate space and produce garbled output.
    TSTree *new_tree = ts_parser_parse_string(_parser, was_edited ? _tree : nullptr,
                                              _src.get_data(), _src_len);
    ERR_FAIL_COND_MSG(!new_tree, "GDScriptTreeSitter: ts_parser_parse_string returned null.");
    if (_tree) ts_tree_delete(_tree);
    _tree = new_tree;
}

Array GDScriptTreeSitter::query(const String &p_pattern) {
    Array out;
    if (_edited || !_tree) return out;

    CharString pat = p_pattern.utf8();
    uint32_t err_offset = 0;
    TSQueryError err_type = TSQueryErrorNone;
    TSQuery *q = ts_query_new(tree_sitter_gdscript(),
                              pat.get_data(), (uint32_t)pat.length(),
                              &err_offset, &err_type);
    ERR_FAIL_COND_V_MSG(!q, out,
        String("GDScriptTreeSitter: query error (type=") + itos((int)err_type)
        + " offset=" + itos((int)err_offset) + ")");

    TSQueryCursor *cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, q, ts_tree_root_node(_tree));

    const char *src = _src.get_data();
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        Dictionary m;
        for (uint32_t i = 0; i < match.capture_count; i++) {
            const TSQueryCapture &cap = match.captures[i];
            uint32_t name_len = 0;
            const char *name = ts_query_capture_name_for_id(q, cap.index, &name_len);
            Dictionary info;
            info["text"]       = ts_text(cap.node, src, _src_len);
            info["start_line"] = (int)ts_node_start_point(cap.node).row;
            info["end_line"]   = (int)ts_node_end_point(cap.node).row;
            info["start_col"]  = (int)ts_node_start_point(cap.node).column;
            info["end_col"]    = (int)ts_node_end_point(cap.node).column;
            m[String::utf8(name, (int)name_len)] = info;
        }
        out.push_back(m);
    }

    ts_query_cursor_delete(cursor);
    ts_query_delete(q);
    return out;
}

} // namespace godot
