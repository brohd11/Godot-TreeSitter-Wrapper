#include "gdscript_tree_query.h"
#include "ts_helpers.h"
#include "gd_keys.h"
#include <godot_cpp/core/class_db.hpp>
#include <tree_sitter/api.h>
#include <cstring>

namespace godot {

// ---------------------------------------------------------------------------
// Parameter list → { name → { type, default, variadic } }
// ---------------------------------------------------------------------------

static Dictionary parse_params(TSNode params, const char *src, uint32_t src_len) {
    const Keys &K = Keys::get();
    Dictionary out;
    if (ts_node_is_null(params)) return out;
    uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count; i++) {
        TSNode p  = ts_node_named_child(params, i);
        const char *t = ts_node_type(p);
        String name;
        Dictionary info;
        info[K.variadic] = false;

        if (strcmp(t, "identifier") == 0) {
            name              = ts_text(p, src, src_len);
            info[K.type]          = String();
            info[K.default_value] = String();
        }
        else if (strcmp(t, "typed_parameter") == 0) {
            name              = first_ident_child(p, src, src_len);
            info[K.type]          = ts_field(p, "type",  src, src_len);
            info[K.default_value] = String();
        }
        else if (strcmp(t, "default_parameter") == 0) {
            name              = first_ident_child(p, src, src_len);
            info[K.type]          = String();
            info[K.default_value] = ts_field(p, "value", src, src_len);
        }
        else if (strcmp(t, "typed_default_parameter") == 0) {
            name              = first_ident_child(p, src, src_len);
            info[K.type]          = ts_field(p, "type",  src, src_len);
            info[K.default_value] = ts_field(p, "value", src, src_len);
        }
        else if (strcmp(t, "variadic_parameter") == 0) {
            uint32_t vc = ts_node_named_child_count(p);
            if (vc > 0) {
                TSNode inner = ts_node_named_child(p, 0);
                const char *it = ts_node_type(inner);
                if (strcmp(it, "identifier") == 0) {
                    name           = ts_text(inner, src, src_len);
                    info[K.type] = String();
                } else {
                    name           = first_ident_child(inner, src, src_len);
                    info[K.type] = ts_field(inner, "type", src, src_len);
                }
            }
            info[K.default_value] = String();
            info[K.variadic]      = true;
        }
        else { continue; }

        if (!name.is_empty()) out[name] = info;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Lambda parsing (mutually recursive with collect_locals)
// ---------------------------------------------------------------------------

static void collect_locals(TSNode node, const char *src, uint32_t src_len,
                             Dictionary &locals);

static Dictionary parse_lambda_info(TSNode lambda, const char *src, uint32_t src_len) {
    const Keys &K = Keys::get();
    Dictionary locals;
    TSNode body_node = ts_field_node(lambda, "body");
    if (!ts_node_is_null(body_node)) {
        uint32_t bc = ts_node_named_child_count(body_node);
        for (uint32_t j = 0; j < bc; j++)
            collect_locals(ts_node_named_child(body_node, j), src, src_len, locals);
    }
    Dictionary info;
    info[K.args]        = parse_params(ts_field_node(lambda, "parameters"), src, src_len);
    info[K.return_type] = ts_field(lambda, "return_type", src, src_len);
    info[K.line]        = (int)ts_node_start_point(lambda).row;
    info[K.end_line]    = (int)ts_node_end_point(lambda).row;
    info[K.locals]      = locals;
    return info;
}

static void maybe_attach_lambda(TSNode var_node, Dictionary &info,
                                 const char *src, uint32_t src_len) {
    TSNode val = ts_field_node(var_node, "value");
    if (!ts_node_is_null(val) && strcmp(ts_node_type(val), "lambda") == 0)
        info[Keys::get().lambda] = parse_lambda_info(val, src, src_len);
}

// Recursively collect local variable_statements inside a function / lambda body.
// Stops at lambda and nested function boundaries.
static void collect_locals(TSNode node, const char *src, uint32_t src_len,
                             Dictionary &locals) {
    const Keys &K = Keys::get();
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
            info[K.keyword] = ts_node_is_null(sf) ? String("var") : String("static var");
            info[K.line]    = (int)ts_node_start_point(node).row;
            info[K.type]    = ts_field(node, "type", src, src_len);
            maybe_attach_lambda(node, info, src, src_len);
            locals[name] = info;
        }
    }
    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; i++)
        collect_locals(ts_node_named_child(node, i), src, src_len, locals);
}

// ---------------------------------------------------------------------------
// Class path navigation
// ---------------------------------------------------------------------------

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
            if (pi == parts.size() - 1) return child;
            TSNode body = ts_field_node(child, "body");
            if (ts_node_is_null(body)) return TSNode{};
            current = body;
            found = true;
            break;
        }
        if (!found) return TSNode{};
    }
    return TSNode{};
}

static TSNode find_class_body(TSNode root, const String &path,
                               const char *src, uint32_t src_len) {
    if (path.is_empty()) return root;
    TSNode def = find_class_def(root, path, src, src_len);
    if (ts_node_is_null(def)) return TSNode{};
    TSNode body = ts_field_node(def, "body");
    return ts_node_is_null(body) ? TSNode{} : body;
}

// Recursively populate `out` with one entry per reachable class scope.
// class_def is null for the root scope, or the class_definition node for inner classes.
// Root entry keys: "extends", "class_name", "line", "end_line"
// Inner entry keys: "extends", "line", "end_line"
static void collect_classes(TSNode body, TSNode class_def,
                              const char *src, uint32_t src_len,
                              const String &prefix, Dictionary &out) {
    const Keys &K = Keys::get();
    Dictionary info;
    if (ts_node_is_null(class_def)) {
        String extends, class_name;
        uint32_t n = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < n; i++) {
            TSNode child = ts_node_named_child(body, i);
            const char *t = ts_node_type(child);
            if (strcmp(t, "extends_statement") == 0)
                extends = extends_from_node(child, src, src_len);
            else if (strcmp(t, "class_name_statement") == 0)
                class_name = ts_field(child, "name", src, src_len);
        }
        info[K.extends]    = extends;
        info[K.class_name] = class_name;
        info[K.line]       = (int)ts_node_start_point(body).row;
        info[K.end_line]   = (int)ts_node_end_point(body).row;
    } else {
        TSNode ext = ts_field_node(class_def, "extends");
        info[K.extends]  = ts_node_is_null(ext) ? String() : extends_from_node(ext, src, src_len);
        info[K.line]     = (int)ts_node_start_point(class_def).row;
        info[K.end_line] = (int)ts_node_end_point(class_def).row;
    }
    out[prefix] = info;

    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        if (strcmp(ts_node_type(child), "class_definition") != 0) continue;
        String name = ts_field(child, "name", src, src_len);
        if (name.is_empty()) continue;
        String new_path = prefix.is_empty() ? name : (prefix + String(".") + name);
        TSNode inner_body = ts_field_node(child, "body");
        if (!ts_node_is_null(inner_body))
            collect_classes(inner_body, child, src, src_len, new_path, out);
    }
}

// ---------------------------------------------------------------------------
// GDScriptTreeQuery
// ---------------------------------------------------------------------------

void GDScriptTreeQuery::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_classes"),               &GDScriptTreeQuery::get_classes);
    ClassDB::bind_method(D_METHOD("get_extends", "path"),       &GDScriptTreeQuery::get_extends);
    ClassDB::bind_method(D_METHOD("get_members",       "path", "changed_only"), &GDScriptTreeQuery::get_members,       DEFVAL(false));
    ClassDB::bind_method(D_METHOD("get_constants",     "path", "changed_only"), &GDScriptTreeQuery::get_constants,     DEFVAL(false));
    ClassDB::bind_method(D_METHOD("get_inner_classes", "path", "changed_only"), &GDScriptTreeQuery::get_inner_classes, DEFVAL(false));
}

Dictionary GDScriptTreeQuery::get_classes() {
    Dictionary out;
    if (_edited || !_tree) return out;
    collect_classes(ts_tree_root_node(_tree), TSNode{}, _src.get_data(), _src_len,
                    String(), out);
    return out;
}

String GDScriptTreeQuery::get_extends(const String &p_path) {
    if (_edited || !_tree) return String();
    const char *src  = _src.get_data();
    TSNode      root = ts_tree_root_node(_tree);

    if (p_path.is_empty()) {
        uint32_t count = ts_node_named_child_count(root);
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_named_child(root, i);
            if (strcmp(ts_node_type(child), "extends_statement") == 0)
                return extends_from_node(child, src, _src_len);
        }
        return String();
    }

    TSNode def = find_class_def(root, p_path, src, _src_len);
    if (ts_node_is_null(def)) return String();
    TSNode ext = ts_field_node(def, "extends");
    return ts_node_is_null(ext) ? String() : extends_from_node(ext, src, _src_len);
}

Dictionary GDScriptTreeQuery::get_members(const String &p_path, bool p_changed_only) {
    const Keys &K = Keys::get();
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
            info[K.keyword] = ts_node_is_null(sf) ? String("var") : String("static var");
            info[K.line]    = (int)ts_node_start_point(child).row;
            info[K.type]    = ts_field(child, "type", src, _src_len);
            info[K.changed] = (bool)ts_node_has_changes(child);
            maybe_attach_lambda(child, info, src, _src_len);
            out[name] = info;
        }
        else if (strcmp(t, "signal_statement") == 0) {
            String name = ts_field(child, "name", src, _src_len);
            if (name.is_empty()) continue;
            Dictionary info;
            info[K.keyword] = String("signal");
            info[K.line]    = (int)ts_node_start_point(child).row;
            info[K.args]    = parse_params(ts_field_node(child, "parameters"), src, _src_len);
            info[K.changed] = (bool)ts_node_has_changes(child);
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
            info[K.keyword]     = has_child_type(child, "static_keyword")
                                      ? String("static func") : String("func");
            info[K.line]        = (int)ts_node_start_point(child).row;
            info[K.end_line]    = (int)ts_node_end_point(child).row;
            info[K.return_type] = ts_field(child, "return_type", src, _src_len);
            info[K.args]        = parse_params(ts_field_node(child, "parameters"), src, _src_len);
            info[K.locals]      = locals;
            info[K.changed]     = (bool)ts_node_has_changes(child);
            out[name] = info;
        }
        else if (strcmp(t, "class_name_statement") == 0) {
            Dictionary info;
            info[K.keyword] = String("class_name");
            info[K.line]    = (int)ts_node_start_point(child).row;
            info[K.name]    = ts_field(child, "name", src, _src_len);
            info[K.changed] = (bool)ts_node_has_changes(child);
            out[K.class_name_marker] = info;
        }
    }
    return out;
}

Dictionary GDScriptTreeQuery::get_constants(const String &p_path, bool p_changed_only) {
    const Keys &K = Keys::get();
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
            info[K.keyword] = String("const");
            info[K.line]    = (int)ts_node_start_point(child).row;
            info[K.type]    = ts_field(child, "type", src, _src_len);
            info[K.changed] = (bool)ts_node_has_changes(child);
            out[name] = info;
        }
        else if (strcmp(t, "enum_definition") == 0) {
            String name = ts_field(child, "name", src, _src_len);
            int    line = (int)ts_node_start_point(child).row;
            String key  = name.is_empty() ? ("@anon_enum_" + itos(line)) : name;
            Dictionary info;
            info[K.keyword] = String("enum");
            info[K.line]    = line;
            info[K.changed] = (bool)ts_node_has_changes(child);
            out[key] = info;
        }
    }
    return out;
}

Dictionary GDScriptTreeQuery::get_inner_classes(const String &p_path, bool p_changed_only) {
    const Keys &K = Keys::get();
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
        info[K.line]    = (int)ts_node_start_point(child).row;
        info[K.extends] = ts_node_is_null(ext_field)
                              ? String()
                              : extends_from_node(ext_field, src, _src_len);
        info[K.changed] = (bool)ts_node_has_changes(child);
        out[name] = info;
    }
    return out;
}

} // namespace godot
