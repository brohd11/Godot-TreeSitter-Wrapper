#include "gdscript_tree_parser.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/array.hpp>
#include <tree_sitter/api.h>
#include <cstring>

extern "C" const TSLanguage *tree_sitter_gdscript();

namespace godot {

// ---------------------------------------------------------------------------
// Low-level helpers
// src_len is always _src.length() — used to clamp OOB reads that can occur
// if a query somehow fires with a stale tree (defense-in-depth; the _edited
// guard in each query method is the primary protection).
// ---------------------------------------------------------------------------

static String ts_text(TSNode n, const char *src, uint32_t src_len) {
    uint32_t s = ts_node_start_byte(n);
    uint32_t e = ts_node_end_byte(n);
    if (s >= src_len) return String();
    if (e > src_len)  e = src_len;
    if (s >= e)       return String();
    return String::utf8(src + s, e - s);
}

static String ts_field(TSNode parent, const char *fname,
                        const char *src, uint32_t src_len) {
    TSNode child = ts_node_child_by_field_name(parent, fname, (uint32_t)strlen(fname));
    return ts_node_is_null(child) ? String() : ts_text(child, src, src_len);
}

static TSNode ts_field_node(TSNode parent, const char *fname) {
    return ts_node_child_by_field_name(parent, fname, (uint32_t)strlen(fname));
}

static bool has_child_type(TSNode n, const char *type) {
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(ts_node_type(ts_node_child(n, i)), type) == 0) return true;
    return false;
}

// Return text of the first "identifier" or "name" child (all children, not
// just named, so anonymous keyword children don't hide it).
static String first_ident_child(TSNode n, const char *src, uint32_t src_len) {
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_child(n, i);
        const char *t = ts_node_type(c);
        if (strcmp(t, "identifier") == 0 || strcmp(t, "name") == 0)
            return ts_text(c, src, src_len);
    }
    return String();
}

static String extends_from_node(TSNode n, const char *src, uint32_t src_len) {
    String s = ts_text(n, src, src_len).strip_edges();
    if (s.begins_with("extends ")) s = s.substr(8).strip_edges();
    return s;
}

// ---------------------------------------------------------------------------
// Parameter list → { name → { type, default, variadic } }
// ---------------------------------------------------------------------------

static Dictionary parse_params(TSNode params, const char *src, uint32_t src_len) {
    Dictionary out;
    if (ts_node_is_null(params)) return out;
    uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count; i++) {
        TSNode p  = ts_node_named_child(params, i);
        const char *t = ts_node_type(p);
        String name;
        Dictionary info;
        info["variadic"] = false;

        if (strcmp(t, "identifier") == 0) {
            name            = ts_text(p, src, src_len);
            info["type"]    = String();
            info["default"] = String();
        }
        else if (strcmp(t, "typed_parameter") == 0) {
            name            = first_ident_child(p, src, src_len);
            info["type"]    = ts_field(p, "type",  src, src_len);
            info["default"] = String();
        }
        else if (strcmp(t, "default_parameter") == 0) {
            name            = first_ident_child(p, src, src_len);
            info["type"]    = String();
            info["default"] = ts_field(p, "value", src, src_len);
        }
        else if (strcmp(t, "typed_default_parameter") == 0) {
            name            = first_ident_child(p, src, src_len);
            info["type"]    = ts_field(p, "type",  src, src_len);
            info["default"] = ts_field(p, "value", src, src_len);
        }
        else if (strcmp(t, "variadic_parameter") == 0) {
            uint32_t vc = ts_node_named_child_count(p);
            if (vc > 0) {
                TSNode inner = ts_node_named_child(p, 0);
                const char *it = ts_node_type(inner);
                // The inner child may itself be a bare identifier (no sub-children)
                // or a typed_parameter — handle both.
                if (strcmp(it, "identifier") == 0) {
                    name         = ts_text(inner, src, src_len);
                    info["type"] = String();
                } else {
                    name         = first_ident_child(inner, src, src_len);
                    info["type"] = ts_field(inner, "type", src, src_len);
                }
            }
            info["default"]  = String();
            info["variadic"] = true;
        }
        else { continue; }

        if (!name.is_empty()) out[name] = info;
    }
    return out;
}

// ---------------------------------------------------------------------------
// parse_lambda_info and collect_locals are mutually recursive:
//   parse_lambda_info → collect_locals  (to gather the lambda's own locals)
//   collect_locals    → maybe_attach_lambda → parse_lambda_info  (for vars whose value is a lambda)
// Forward declaration breaks the cycle.
// ---------------------------------------------------------------------------

static void collect_locals(TSNode node, const char *src, uint32_t src_len,
                             Dictionary &locals);

// Builds a dict with the same shape as a function member: args, return_type,
// line, end_line, locals.  locals is populated recursively so nested lambdas
// inside this lambda's body each get their own "lambda" field as well.
static Dictionary parse_lambda_info(TSNode lambda, const char *src, uint32_t src_len) {
    Dictionary locals;
    TSNode body_node = ts_field_node(lambda, "body");
    if (!ts_node_is_null(body_node)) {
        uint32_t bc = ts_node_named_child_count(body_node);
        for (uint32_t j = 0; j < bc; j++)
            collect_locals(ts_node_named_child(body_node, j), src, src_len, locals);
    }
    Dictionary info;
    info["args"]        = parse_params(ts_field_node(lambda, "parameters"), src, src_len);
    info["return_type"] = ts_field(lambda, "return_type", src, src_len);
    info["line"]        = (int)ts_node_start_point(lambda).row;
    info["end_line"]    = (int)ts_node_end_point(lambda).row;
    info["locals"]      = locals;
    return info;
}

// Attaches a "lambda" key to info if the var node's value is a lambda.
static void maybe_attach_lambda(TSNode var_node, Dictionary &info,
                                 const char *src, uint32_t src_len) {
    TSNode val = ts_field_node(var_node, "value");
    if (!ts_node_is_null(val) && strcmp(ts_node_type(val), "lambda") == 0)
        info["lambda"] = parse_lambda_info(val, src, src_len);
}

// Recursively collect local variable_statements inside a function / lambda body.
// Stops at lambda boundaries — the lambda's internals are collected by
// parse_lambda_info when it processes the variable that holds the lambda.
// function_definition / constructor_definition guards are dead code in valid
// GDScript (no nested named funcs), but kept defensively.
static void collect_locals(TSNode node, const char *src, uint32_t src_len,
                             Dictionary &locals) {
    const char *t = ts_node_type(node);
    if (strcmp(t, "function_definition") == 0 ||
        strcmp(t, "constructor_definition") == 0 ||
        strcmp(t, "lambda") == 0) return;

    bool is_var = strcmp(t, "variable_statement") == 0 ||
                  strcmp(t, "export_variable_statement") == 0 ||
                  strcmp(t, "onready_variable_statement") == 0;
    if (is_var) {
        String name = ts_field(node, "name", src, src_len);
        if (!name.is_empty()) {
            TSNode sf = ts_field_node(node, "static");
            Dictionary info;
            info["keyword"] = ts_node_is_null(sf) ? String("var") : String("static var");
            info["line"]    = (int)ts_node_start_point(node).row;
            info["type"]    = ts_field(node, "type", src, src_len);
            maybe_attach_lambda(node, info, src, src_len);
            locals[name] = info;
        }
    }
    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; i++)
        collect_locals(ts_node_named_child(node, i), src, src_len, locals);
}

// ---------------------------------------------------------------------------
// Tree navigation helpers
// ---------------------------------------------------------------------------

// Returns the class_definition TSNode for the *last* segment of path.
// For an empty path returns a null TSNode (caller handles root separately).
static TSNode find_class_def(TSNode root, const String &path,
                               const char *src, uint32_t src_len) {
    if (path.is_empty()) return TSNode{};

    PackedStringArray parts = path.split(".");
    TSNode current = root;

    for (int pi = 0; pi < parts.size(); pi++) {
        const String &part = parts[pi];
        uint32_t count = ts_node_named_child_count(current);
        bool found = false;
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_named_child(current, i);
            if (strcmp(ts_node_type(child), "class_definition") != 0) continue;
            if (ts_field(child, "name", src, src_len) != part) continue;
            if (pi == parts.size() - 1) return child; // last segment → return def node
            TSNode body = ts_field_node(child, "body");
            if (ts_node_is_null(body)) return TSNode{}; // path broken
            current = body;
            found = true;
            break;
        }
        if (!found) return TSNode{};
    }
    return TSNode{}; // unreachable but satisfies compiler
}

// Returns the body TSNode to iterate over for a given access path.
// "" → root source node; "Inner" → class_definition("Inner").body.
static TSNode find_class_body(TSNode root, const String &path,
                               const char *src, uint32_t src_len) {
    if (path.is_empty()) return root;
    TSNode def = find_class_def(root, path, src, src_len);
    if (ts_node_is_null(def)) return TSNode{};
    TSNode body = ts_field_node(def, "body");
    return ts_node_is_null(body) ? TSNode{} : body;
}

// ---------------------------------------------------------------------------
// Collect all class access paths reachable from a body node (recursive).
// ---------------------------------------------------------------------------

static void collect_class_paths(TSNode body, const char *src, uint32_t src_len,
                                  const String &prefix, Array &out) {
    out.push_back(prefix);
    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        if (strcmp(ts_node_type(child), "class_definition") != 0) continue;
        String name = ts_field(child, "name", src, src_len);
        if (name.is_empty()) continue;
        String new_path = prefix.is_empty() ? name : (prefix + String(".") + name);
        TSNode inner_body = ts_field_node(child, "body");
        if (!ts_node_is_null(inner_body))
            collect_class_paths(inner_body, src, src_len, new_path, out);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

GDScriptTreeParser::GDScriptTreeParser() {
    _parser = ts_parser_new();
    ERR_FAIL_COND_MSG(
        !ts_parser_set_language(_parser, tree_sitter_gdscript()),
        "GDScriptTreeParser: language ABI mismatch — rebuild the GDExtension.");
}

GDScriptTreeParser::~GDScriptTreeParser() {
    if (_tree)   { ts_tree_delete(_tree);     _tree   = nullptr; }
    if (_parser) { ts_parser_delete(_parser); _parser = nullptr; }
}

void GDScriptTreeParser::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open_text", "text"),    &GDScriptTreeParser::open_text);
    ClassDB::bind_method(D_METHOD("open_file", "path"),    &GDScriptTreeParser::open_file);
    ClassDB::bind_method(D_METHOD("reparse_text", "text"), &GDScriptTreeParser::reparse_text);
    ClassDB::bind_method(
        D_METHOD("apply_edit",
                 "start_byte",  "old_end_byte",  "new_end_byte",
                 "start_row",   "start_col",
                 "old_end_row", "old_end_col",
                 "new_end_row", "new_end_col"),
        &GDScriptTreeParser::apply_edit);

    ClassDB::bind_method(D_METHOD("get_class_paths"),           &GDScriptTreeParser::get_class_paths);
    ClassDB::bind_method(D_METHOD("get_extends", "path"),       &GDScriptTreeParser::get_extends);
    ClassDB::bind_method(D_METHOD("get_members",       "path", "changed_only"), &GDScriptTreeParser::get_members,       DEFVAL(false));
    ClassDB::bind_method(D_METHOD("get_constants",     "path", "changed_only"), &GDScriptTreeParser::get_constants,     DEFVAL(false));
    ClassDB::bind_method(D_METHOD("get_inner_classes", "path", "changed_only"), &GDScriptTreeParser::get_inner_classes, DEFVAL(false));
}

// ---------------------------------------------------------------------------
// Open / reparse
// ---------------------------------------------------------------------------

void GDScriptTreeParser::open_text(const String &p_text) {
    _src     = p_text.utf8();
    _src_len = (uint32_t)_src.length();
    _edited  = false;
    TSTree *new_tree = ts_parser_parse_string(_parser, nullptr,
                                              _src.get_data(), _src_len);
    ERR_FAIL_COND_MSG(!new_tree, "GDScriptTreeParser: ts_parser_parse_string returned null.");
    if (_tree) ts_tree_delete(_tree);
    _tree = new_tree;
}

bool GDScriptTreeParser::open_file(const String &p_path) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
    ERR_FAIL_COND_V_MSG(!f.is_valid(), false,
        "GDScriptTreeParser: cannot open file: " + p_path);
    open_text(f->get_as_text());
    return true;
}

void GDScriptTreeParser::apply_edit(int p_start_byte,
                                     int p_old_end_byte, int p_new_end_byte,
                                     int p_start_row,    int p_start_col,
                                     int p_old_end_row,  int p_old_end_col,
                                     int p_new_end_row,  int p_new_end_col) {
    if (!_tree) return;
    // Negative values would silently become huge uint32_t — reject them.
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
    _edited = true; // queries are unsafe until reparse_text is called
}

void GDScriptTreeParser::reparse_text(const String &p_text) {
    _src     = p_text.utf8();
    _src_len = (uint32_t)_src.length();
    _edited  = false;
    TSTree *new_tree = ts_parser_parse_string(_parser, _tree,
                                              _src.get_data(), _src_len);
    ERR_FAIL_COND_MSG(!new_tree, "GDScriptTreeParser: ts_parser_parse_string returned null.");
    if (_tree) ts_tree_delete(_tree);
    _tree = new_tree;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Array GDScriptTreeParser::get_class_paths() {
    Array out;
    if (_edited || !_tree) return out;
    collect_class_paths(ts_tree_root_node(_tree), _src.get_data(), _src_len,
                        String(), out);
    return out;
}

String GDScriptTreeParser::get_extends(const String &p_path) {
    if (_edited || !_tree) return String();
    const char   *src     = _src.get_data();
    TSNode        root    = ts_tree_root_node(_tree);

    if (p_path.is_empty()) {
        // Root: standalone extends_statement is a direct child of source.
        uint32_t count = ts_node_named_child_count(root);
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_named_child(root, i);
            if (strcmp(ts_node_type(child), "extends_statement") == 0)
                return extends_from_node(child, src, _src_len);
        }
        return String();
    }

    // Inner class: extends is the "extends" field on the class_definition node.
    TSNode def = find_class_def(root, p_path, src, _src_len);
    if (ts_node_is_null(def)) return String();
    TSNode ext = ts_field_node(def, "extends");
    return ts_node_is_null(ext) ? String() : extends_from_node(ext, src, _src_len);
}

Dictionary GDScriptTreeParser::get_members(const String &p_path, bool p_changed_only) {
    Dictionary out;
    if (_edited || !_tree) return out;
    const char *src  = _src.get_data();
    TSNode body = find_class_body(ts_tree_root_node(_tree), p_path, src, _src_len);
    if (ts_node_is_null(body)) return out;

    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        const char *t = ts_node_type(child);
        if (p_changed_only && !ts_node_has_changes(child)) continue;

        if (strcmp(t, "variable_statement") == 0 ||
            strcmp(t, "export_variable_statement") == 0 ||
            strcmp(t, "onready_variable_statement") == 0) {
            String name = ts_field(child, "name", src, _src_len);
            if (name.is_empty()) continue;
            TSNode sf = ts_field_node(child, "static");
            Dictionary info;
            info["keyword"] = ts_node_is_null(sf) ? String("var") : String("static var");
            info["line"]    = (int)ts_node_start_point(child).row;
            info["type"]    = ts_field(child, "type", src, _src_len);
            info["changed"] = (bool)ts_node_has_changes(child);
            maybe_attach_lambda(child, info, src, _src_len);
            out[name] = info;
        }
        else if (strcmp(t, "signal_statement") == 0) {
            String name = ts_field(child, "name", src, _src_len);
            if (name.is_empty()) continue;
            Dictionary info;
            info["keyword"] = String("signal");
            info["line"]    = (int)ts_node_start_point(child).row;
            info["args"]    = parse_params(ts_field_node(child, "parameters"), src, _src_len);
            info["changed"] = (bool)ts_node_has_changes(child);
            out[name] = info;
        }
        else if (strcmp(t, "function_definition") == 0 ||
                 strcmp(t, "constructor_definition") == 0) {
            bool is_ctor = strcmp(t, "constructor_definition") == 0;
            String name = is_ctor ? String("_init") : ts_field(child, "name", src, _src_len);
            if (name.is_empty()) continue;

            TSNode body_node = ts_field_node(child, "body");
            Dictionary locals;
            if (!ts_node_is_null(body_node)) {
                uint32_t bc = ts_node_named_child_count(body_node);
                for (uint32_t j = 0; j < bc; j++)
                    collect_locals(ts_node_named_child(body_node, j), src, _src_len, locals);
            }
            Dictionary info;
            info["keyword"]     = has_child_type(child, "static_keyword")
                                      ? String("static func") : String("func");
            info["line"]        = (int)ts_node_start_point(child).row;
            info["end_line"]    = (int)ts_node_end_point(child).row;
            info["return_type"] = ts_field(child, "return_type", src, _src_len);
            info["args"]        = parse_params(ts_field_node(child, "parameters"), src, _src_len);
            info["locals"]      = locals;
            info["changed"]     = (bool)ts_node_has_changes(child);
            out[name] = info;
        }
        else if (strcmp(t, "class_name_statement") == 0) {
            Dictionary info;
            info["keyword"] = String("class_name");
            info["line"]    = (int)ts_node_start_point(child).row;
            info["name"]    = ts_field(child, "name", src, _src_len);
            info["changed"] = (bool)ts_node_has_changes(child);
            // Double-underscore sentinel: won't collide with a valid GDScript identifier.
            out["__class_name__"] = info;
        }
    }
    return out;
}

Dictionary GDScriptTreeParser::get_constants(const String &p_path, bool p_changed_only) {
    Dictionary out;
    if (_edited || !_tree) return out;
    const char *src  = _src.get_data();
    TSNode body = find_class_body(ts_tree_root_node(_tree), p_path, src, _src_len);
    if (ts_node_is_null(body)) return out;

    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        const char *t = ts_node_type(child);
        if (p_changed_only && !ts_node_has_changes(child)) continue;

        if (strcmp(t, "const_statement") == 0) {
            String name = ts_field(child, "name", src, _src_len);
            if (name.is_empty()) continue;
            Dictionary info;
            info["keyword"] = String("const");
            info["line"]    = (int)ts_node_start_point(child).row;
            info["type"]    = ts_field(child, "type", src, _src_len);
            info["changed"] = (bool)ts_node_has_changes(child);
            out[name] = info;
        }
        else if (strcmp(t, "enum_definition") == 0) {
            String name = ts_field(child, "name", src, _src_len);
            int    line = (int)ts_node_start_point(child).row;
            // Anonymous enums get a unique key based on their line number.
            String key  = name.is_empty()
                              ? ("@anon_enum_" + itos(line))
                              : name;
            Dictionary info;
            info["keyword"] = String("enum");
            info["line"]    = line;
            info["changed"] = (bool)ts_node_has_changes(child);
            out[key] = info;
        }
    }
    return out;
}

Dictionary GDScriptTreeParser::get_inner_classes(const String &p_path, bool p_changed_only) {
    Dictionary out;
    if (_edited || !_tree) return out;
    const char *src  = _src.get_data();
    TSNode body = find_class_body(ts_tree_root_node(_tree), p_path, src, _src_len);
    if (ts_node_is_null(body)) return out;

    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        if (strcmp(ts_node_type(child), "class_definition") != 0) continue;
        if (p_changed_only && !ts_node_has_changes(child)) continue;
        String name = ts_field(child, "name", src, _src_len);
        if (name.is_empty()) continue;

        TSNode ext_field = ts_field_node(child, "extends");
        Dictionary info;
        info["line"]    = (int)ts_node_start_point(child).row;
        info["extends"] = ts_node_is_null(ext_field)
                              ? String()
                              : extends_from_node(ext_field, src, _src_len);
        info["changed"] = (bool)ts_node_has_changes(child);
        out[name] = info;
    }
    return out;
}

} // namespace godot
