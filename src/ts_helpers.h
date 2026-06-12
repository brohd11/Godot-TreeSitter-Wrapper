#pragma once
#include <godot_cpp/variant/string.hpp>
#include <tree_sitter/api.h>
#include <cstring>

// Internal tree-sitter node helpers shared across translation units.
// All functions are inline to avoid ODR issues when included in multiple .cpp files.

namespace godot {

inline String ts_text(TSNode n, const char *src, uint32_t src_len) {
    uint32_t s = ts_node_start_byte(n);
    uint32_t e = ts_node_end_byte(n);
    if (s >= src_len) return String();
    if (e > src_len)  e = src_len;
    if (s >= e)       return String();
    return String::utf8(src + s, e - s);
}

inline String ts_field(TSNode parent, const char *fname,
                       const char *src, uint32_t src_len) {
    TSNode child = ts_node_child_by_field_name(parent, fname, (uint32_t)strlen(fname));
    return ts_node_is_null(child) ? String() : ts_text(child, src, src_len);
}

inline TSNode ts_field_node(TSNode parent, const char *fname) {
    return ts_node_child_by_field_name(parent, fname, (uint32_t)strlen(fname));
}

inline bool has_child_type(TSNode n, const char *type) {
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(ts_node_type(ts_node_child(n, i)), type) == 0) return true;
    return false;
}

inline String first_ident_child(TSNode n, const char *src, uint32_t src_len) {
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_child(n, i);
        const char *t = ts_node_type(c);
        if (strcmp(t, "identifier") == 0 || strcmp(t, "name") == 0)
            return ts_text(c, src, src_len);
    }
    return String();
}

inline String extends_from_node(TSNode n, const char *src, uint32_t src_len) {
    String s = ts_text(n, src, src_len).strip_edges();
    if (s.begins_with("extends ")) s = s.substr(8).strip_edges();
    return s;
}

} // namespace godot
