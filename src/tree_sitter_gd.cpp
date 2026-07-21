#include "tree_sitter_gd.h"
#include "ts_helpers.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <tree_sitter/api.h>
#include <cstdlib>

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
    ClassDB::bind_method(D_METHOD("update_text", "new_text"), &GDScriptTreeSitter::update_text);
    ClassDB::bind_method(
        D_METHOD("apply_edit",
                 "start_byte",  "old_end_byte",  "new_end_byte",
                 "start_row",   "start_col",
                 "old_end_row", "old_end_col",
                 "new_end_row", "new_end_col"),
        &GDScriptTreeSitter::apply_edit);
    ClassDB::bind_method(D_METHOD("query", "pattern"), &GDScriptTreeSitter::query);
    ClassDB::bind_method(D_METHOD("set_bracket_mode", "enabled"), &GDScriptTreeSitter::set_bracket_mode);
    ClassDB::bind_method(D_METHOD("get_bracket_mode"), &GDScriptTreeSitter::get_bracket_mode);
    ClassDB::bind_method(D_METHOD("get_brackets"), &GDScriptTreeSitter::get_brackets);
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
    if (_bracket_mode) {
        _rebuild_line_starts();
        _brackets_full_scan();
    }
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
    if (_bracket_mode) {
        _rebuild_line_starts();
        _brackets_full_scan();
    }
}

// Advance a TSPoint from `start` over the bytes buf[from, to). Column is a byte
// offset within the line, matching tree-sitter's TSPoint.column convention.
static TSPoint advance_point(const char *buf, uint32_t from, uint32_t to, TSPoint start) {
    TSPoint p = start;
    for (uint32_t i = from; i < to; i++) {
        if (buf[i] == '\n') { p.row++; p.column = 0; }
        else                  p.column++;
    }
    return p;
}

void GDScriptTreeSitter::update_text(const String &p_new_text) {
    // No prior tree (or a previous parse failed): fall back to a full parse.
    if (!_tree) { open_text(p_new_text); return; }

    CharString new_src = p_new_text.utf8();
    uint32_t    new_len = (uint32_t)new_src.length();
    const char *old_buf = _src.get_data();
    const char *new_buf = new_src.get_data();
    uint32_t    old_len = _src_len;

    // Longest common prefix (in bytes).
    uint32_t sb = 0;
    while (sb < old_len && sb < new_len && old_buf[sb] == new_buf[sb]) sb++;
    // Longest common suffix, not crossing the prefix.
    uint32_t oeb = old_len, neb = new_len;
    while (oeb > sb && neb > sb && old_buf[oeb - 1] == new_buf[neb - 1]) { oeb--; neb--; }

    // Points: prefix is shared, so the start point is identical in both buffers.
    TSPoint sp  = advance_point(old_buf, 0,  sb,  TSPoint{0, 0});
    TSPoint oep = advance_point(old_buf, sb, oeb, sp);
    TSPoint nep = advance_point(new_buf, sb, neb, sp);

    TSInputEdit edit;
    edit.start_byte    = sb;
    edit.old_end_byte  = oeb;
    edit.new_end_byte  = neb;
    edit.start_point   = sp;
    edit.old_end_point = oep;
    edit.new_end_point = nep;
    ts_tree_edit(_tree, &edit);

    TSTree *old_tree = _tree;
    _src     = new_src;
    _src_len = new_len;
    _edited  = false;
    TSTree *new_tree = ts_parser_parse_string(_parser, old_tree,
                                              _src.get_data(), _src_len);
    ERR_FAIL_COND_MSG(!new_tree, "GDScriptTreeSitter: ts_parser_parse_string returned null.");

    if (_bracket_mode) {
        _rebuild_line_starts();
        // Changed ranges must be read while both trees are alive. They cover
        // structural fall-out beyond the byte diff (e.g. an unmatched quote
        // restringing everything below), so the bracket re-scan stays correct.
        uint32_t range_count = 0;
        TSRange *ranges = ts_tree_get_changed_ranges(old_tree, new_tree, &range_count);
        _brackets_after_edit(sp.row, oep.row, nep.row, ranges, range_count, new_tree);
        ::free(ranges); // :: — GDCLASS injects a member named `free`
    }

    ts_tree_delete(old_tree);
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

// ---------------------------------------------------------------------------
// Bracket mode — incrementally maintained { line: { column: depth } } map
// ---------------------------------------------------------------------------

void GDScriptTreeSitter::set_bracket_mode(bool p_enabled) {
    if (p_enabled == _bracket_mode) return;
    _bracket_mode = p_enabled;
    if (!_bracket_mode) {
        _bracket_lines.clear();
        _line_starts.clear();
        return;
    }
    if (_tree && !_edited) {
        _rebuild_line_starts();
        _brackets_full_scan();
    }
}

bool GDScriptTreeSitter::get_bracket_mode() const {
    return _bracket_mode;
}

Dictionary GDScriptTreeSitter::get_brackets() const {
    Dictionary out;
    if (_edited || !_tree || !_bracket_mode) return out;
    for (size_t row = 0; row < _bracket_lines.size(); row++) {
        const BracketLine &bl = _bracket_lines[row];
        if (bl.entries.empty()) continue;
        Dictionary line_map;
        for (const auto &e : bl.entries) line_map[e.first] = e.second;
        out[(int64_t)row] = line_map;
    }
    return out;
}

void GDScriptTreeSitter::_rebuild_line_starts() {
    _line_starts.clear();
    _line_starts.push_back(0);
    const char *src = _src.get_data();
    for (uint32_t b = 0; b < _src_len; b++)
        if (src[b] == '\n') _line_starts.push_back(b + 1);
}

// Cursor walk over `scope` collecting ()[]{} tokens on rows [from_row, to_row]
// into _bracket_lines, with running depth starting at start_depth. String and
// comment contents are not tokens, so only real brackets are seen. Depth is
// raw (not clamped at 0) so per-line end_depth stays exact for incremental
// splices. Rows outside the range are ignored entirely — start_depth already
// subsumes everything above from_row.
void GDScriptTreeSitter::_scan_brackets(TSNode scope, int32_t start_depth,
                                        uint32_t from_row, uint32_t to_row) {
    const char *src = _src.get_data();
    int32_t depth = start_depth;
    uint32_t last_row = from_row;

    TSTreeCursor cursor = ts_tree_cursor_new(scope);
    bool descending = true;
    while (true) {
        if (descending) {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_named(node)) {
                const char *t = ts_node_type(node);
                if (t[0] != '\0' && t[1] == '\0') { // single-char anonymous token
                    bool opener = t[0] == '(' || t[0] == '[' || t[0] == '{';
                    bool closer = t[0] == ')' || t[0] == ']' || t[0] == '}';
                    if (opener || closer) {
                        uint32_t row = ts_node_start_point(node).row;
                        if (row >= from_row && row <= to_row) {
                            // Depth before this token = end depth of every row
                            // since the last bracket seen (from_row included:
                            // depth still equals start_depth there).
                            if (row > last_row) {
                                for (uint32_t r = last_row; r < row; r++)
                                    _bracket_lines[r].end_depth = depth;
                                last_row = row;
                            }

                            if (closer) depth--;

                            // Character column: count UTF-8 lead bytes from line start.
                            uint32_t byte = ts_node_start_byte(node);
                            int32_t col = 0;
                            for (uint32_t b = _line_starts[row]; b < byte; b++)
                                if ((src[b] & 0xC0) != 0x80) col++;
                            _bracket_lines[row].entries.push_back({ col, depth });

                            if (opener) depth++;
                        }
                    }
                }
            }
        }
        if (descending && ts_tree_cursor_goto_first_child(&cursor)) continue;
        descending = false;
        if (ts_tree_cursor_goto_next_sibling(&cursor)) { descending = true; continue; }
        if (!ts_tree_cursor_goto_parent(&cursor)) break;
    }
    ts_tree_cursor_delete(&cursor);

    for (uint32_t r = last_row; r <= to_row; r++)
        _bracket_lines[r].end_depth = depth;
}

void GDScriptTreeSitter::_brackets_full_scan() {
    _bracket_lines.assign(_line_starts.size(), BracketLine{});
    if (!_tree || _line_starts.empty()) return;
    _scan_brackets(ts_tree_root_node(_tree), 0, 0, (uint32_t)_line_starts.size() - 1);
}

void GDScriptTreeSitter::_brackets_after_edit(uint32_t start_row, uint32_t old_end_row,
                                              uint32_t new_end_row,
                                              const TSRange *ranges, uint32_t range_count,
                                              TSTree *p_new_tree) {
    if (_bracket_lines.empty()) { _brackets_full_scan(); return; }

    // Rows to re-scan, in new coordinates: the byte-diff region plus any
    // changed range (structural fall-out beyond the text edit).
    uint32_t r0 = start_row;
    uint32_t r1 = new_end_row;
    for (uint32_t i = 0; i < range_count; i++) {
        if (ranges[i].start_point.row < r0) r0 = ranges[i].start_point.row;
        if (ranges[i].end_point.row   > r1) r1 = ranges[i].end_point.row;
    }
    int32_t row_delta = (int32_t)new_end_row - (int32_t)old_end_row;
    uint32_t old_r1 = (uint32_t)((int32_t)r1 - row_delta); // propagation doesn't move rows

    int32_t start_depth   = r0 > 0 ? _bracket_lines[r0 - 1].end_depth : 0;
    int32_t old_end_depth = _bracket_lines[old_r1].end_depth;

    _bracket_lines.erase(_bracket_lines.begin() + r0, _bracket_lines.begin() + old_r1 + 1);
    _bracket_lines.insert(_bracket_lines.begin() + r0, r1 - r0 + 1, BracketLine{});

    uint32_t from_byte = _line_starts[r0];
    uint32_t to_byte   = (r1 + 1 < _line_starts.size()) ? _line_starts[r1 + 1] : _src_len;
    if (to_byte > from_byte) {
        TSNode scope = ts_node_descendant_for_byte_range(ts_tree_root_node(p_new_tree),
                                                         from_byte, to_byte);
        _scan_brackets(scope, start_depth, r0, r1);
    } else {
        for (uint32_t r = r0; r <= r1; r++) _bracket_lines[r].end_depth = start_depth;
    }

    // If the rescan changed the depth at the end of the region, shift every
    // bracket below by the same delta (raw depths make this exact).
    int32_t delta = _bracket_lines[r1].end_depth - old_end_depth;
    if (delta != 0) {
        for (size_t r = r1 + 1; r < _bracket_lines.size(); r++) {
            for (auto &e : _bracket_lines[r].entries) e.second += delta;
            _bracket_lines[r].end_depth += delta;
        }
    }
}

} // namespace godot
