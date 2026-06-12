#include "gdscript_tree_parser.h"
#include "ts_helpers.h"
#include <godot_cpp/core/class_db.hpp>
#include <tree_sitter/api.h>
#include <cstring>

namespace godot {

// ---------------------------------------------------------------------------
// Dictionary keys, interned as StringName once and reused for every entry.
// Access through Keys::get() (aliased to `K` inside each function).
// ---------------------------------------------------------------------------

struct Keys {
    StringName member_type     = "member_type";
    StringName member_name     = "member_name";
    StringName type            = "type";
    StringName has_static_type = "has_static_type";
    StringName assignment      = "assignment";
    StringName default_value   = "default";
    StringName variadic        = "variadic";
    StringName line_index      = "line_index";
    StringName column_index    = "column_index";
    StringName line            = "line";
    StringName end_line        = "end_line";
    StringName access_path     = "access_path";
    StringName script_path     = "script_path";
    StringName extends         = "extends";
    StringName class_name      = "class_name";
    StringName keyword         = "keyword";
    StringName changed         = "changed";
    StringName name            = "name";
    StringName args            = "args";
    StringName return_type     = "return_type";
    StringName locals          = "locals";
    StringName lambda          = "lambda";
    StringName members         = "members";
    StringName constants       = "constants";
    StringName inner_classes   = "inner_classes";
    StringName class_name_marker = "__class_name__";

    static const Keys &get() {
        static Keys k;
        return k;
    }
};

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
// parse_script() — single-pass full extraction
// ---------------------------------------------------------------------------

static StringName to_string_name(const String &s) {
    return s.is_empty() ? StringName() : StringName(s);
}

static StringName check_type(const String &s) {
    return s.begins_with(":") && s.ends_with("=") ? StringName() : StringName(s);
}

// Stamp the six fields every member dictionary shares; callers add their own extras.
// member_type / member_name / access_path / script_path are stored as StringName.
static Dictionary make_member(const StringName &member_type, const String &member_name,
                              TSNode node, const String &access_path,
                              const String &script_path) {
    const Keys &K = Keys::get();
    Dictionary d;
    d[K.member_type]  = member_type;
    d[K.member_name]  = to_string_name(member_name);
    d[K.line_index]   = (int)ts_node_start_point(node).row;
    d[K.column_index] = (int)ts_node_start_point(node).column;
    d[K.access_path]  = to_string_name(access_path);
    d[K.script_path]  = to_string_name(script_path);
    return d;
}


// Forward declaration: collect_locals_v2 and parse_lambda_info_v2 are mutually recursive.
static void collect_locals_v2(TSNode node, const char *src, uint32_t src_len,
                               const String &access_path, const String &script_path,
                               Dictionary &locals);

static Dictionary parse_params_v2(TSNode params, const char *src, uint32_t src_len,
                                    const String &access_path, const String &script_path) {
    const Keys &K = Keys::get();
    Dictionary out;
    if (ts_node_is_null(params)) return out;
    uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count; i++) {
        TSNode p = ts_node_named_child(params, i);
        const char *t = ts_node_type(p);
        String name, type, default_val;
        bool variadic = false;

        if (strcmp(t, "identifier") == 0) {
            name = ts_text(p, src, src_len);
        } else if (strcmp(t, "typed_parameter") == 0) {
            name = first_ident_child(p, src, src_len);
            type = ts_field(p, "type", src, src_len);
        } else if (strcmp(t, "default_parameter") == 0) {
            name        = first_ident_child(p, src, src_len);
            default_val = ts_field(p, "value", src, src_len);
        } else if (strcmp(t, "typed_default_parameter") == 0) {
            name        = first_ident_child(p, src, src_len);
            type        = ts_field(p, "type",  src, src_len);
            default_val = ts_field(p, "value", src, src_len);
        } else if (strcmp(t, "variadic_parameter") == 0) {
            uint32_t vc = ts_node_named_child_count(p);
            if (vc > 0) {
                TSNode inner = ts_node_named_child(p, 0);
                if (strcmp(ts_node_type(inner), "identifier") == 0) {
                    name = ts_text(inner, src, src_len);
                } else {
                    name = first_ident_child(inner, src, src_len);
                    type = ts_field(inner, "type", src, src_len);
                }
            }
            variadic = true;
        } else { continue; }

        if (name.is_empty()) continue;
        Dictionary info = make_member(StringName("func_arg"), name, p, access_path, script_path);
        info[K.type]            = to_string_name(type);
        info[K.has_static_type] = !type.is_empty();
        info[K.default_value]   = default_val;
        info[K.variadic]        = variadic;
        out[name] = info;
    }
    return out;
}

static Dictionary parse_lambda_info_v2(TSNode lambda, const char *src, uint32_t src_len,
                                         const String &access_path, const String &script_path) {
    const Keys &K = Keys::get();
    Dictionary locals;
    TSNode body_node = ts_field_node(lambda, "body");
    if (!ts_node_is_null(body_node)) {
        uint32_t bc = ts_node_named_child_count(body_node);
        for (uint32_t j = 0; j < bc; j++)
            collect_locals_v2(ts_node_named_child(body_node, j), src, src_len, access_path, script_path, locals);
    }
    Dictionary info;
    info[K.args]        = parse_params_v2(ts_field_node(lambda, "parameters"), src, src_len, access_path, script_path);
    info[K.return_type] = to_string_name(ts_field(lambda, "return_type", src, src_len));
    info[K.line_index]  = (int)ts_node_start_point(lambda).row;
    info[K.end_line]    = (int)ts_node_end_point(lambda).row;
    info[K.locals]      = locals;
    return info;
}

static void maybe_attach_lambda_v2(TSNode var_node, Dictionary &info,
                                    const char *src, uint32_t src_len,
                                    const String &access_path, const String &script_path) {
    TSNode val = ts_field_node(var_node, "value");
    if (!ts_node_is_null(val) && strcmp(ts_node_type(val), "lambda") == 0)
        info[Keys::get().lambda] = parse_lambda_info_v2(val, src, src_len, access_path, script_path);
}

static void collect_locals_v2(TSNode node, const char *src, uint32_t src_len,
                               const String &access_path, const String &script_path,
                               Dictionary &locals) {
    const Keys &K = Keys::get();
    const char *t = ts_node_type(node);
    if (strcmp(t, "function_definition") == 0 ||
        strcmp(t, "constructor_definition") == 0 ||
        strcmp(t, "lambda") == 0) return;

    if (strcmp(t, "variable_statement") == 0 ||
        strcmp(t, "export_variable_statement") == 0 ||
        strcmp(t, "onready_variable_statement") == 0) {
        String name = ts_field(node, "name", src, src_len);
        if (!name.is_empty()) {
            TSNode sf = ts_field_node(node, "static");
            TSNode type_node = ts_field_node(node, "type");
            String type;
            if (!ts_node_is_null(type_node) &&
                strcmp(ts_node_type(type_node), "inferred_type") != 0) {
                type = ts_text(type_node, src, src_len);
            }
            TSNode val_node = ts_field_node(node, "value");
            int line_idx = (int)ts_node_start_point(node).row;
            int col_idx = (int)ts_node_start_point(node).column;
            Dictionary info = make_member(ts_node_is_null(sf) ? StringName("var") : StringName("static var"),
                                          name, node, access_path, script_path);
            info[K.type]            = to_string_name(type);
            info[K.has_static_type] = !type.is_empty();
            info[K.assignment]      = ts_node_is_null(val_node) ? String() : ts_text(val_node, src, src_len);
            maybe_attach_lambda_v2(node, info, src, src_len, access_path, script_path);
            locals[name + String("-") + itos(line_idx) + String("-") + itos(col_idx)] = info;
        }
    } else if (strcmp(t, "for_statement") == 0) {
        TSNode left = ts_field_node(node, "left");
        if (!ts_node_is_null(left)) {
            const char *lt = ts_node_type(left);
            String name, type;
            if (strcmp(lt, "identifier") == 0) {
                name = ts_text(left, src, src_len);
            } else if (strcmp(lt, "typed_parameter") == 0) {
                name = first_ident_child(left, src, src_len);
                type = ts_field(left, "type", src, src_len);
            }
            TSNode right = ts_field_node(node, "right");

            if (!name.is_empty()) {
                int line_idx = (int)ts_node_start_point(node).row;
                int col_idx = (int)ts_node_start_point(node).column;
                Dictionary info = make_member(StringName("for"), name, node, access_path, script_path);
                info[K.type]            = to_string_name(type);
                info[K.has_static_type] = !type.is_empty();
                if (!ts_node_is_null(right)) {
                    info[K.assignment]  = ts_text(right, src, src_len);
                } else {
                    info[K.assignment]  = String();
                }
                locals[name + String("-") + itos(line_idx) + String("-") + itos(col_idx)] = info;
            }
        }
    }

    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; i++)
        collect_locals_v2(ts_node_named_child(node, i), src, src_len, access_path, script_path, locals);
}

static void collect_script(TSNode body, TSNode class_def,
                             const char *src, uint32_t src_len,
                             const String &prefix, const String &script_path,
                             const Dictionary &inherited_constants, Dictionary &out) {
    const Keys &K = Keys::get();
    String scope_name;
    if (!prefix.is_empty()) {
        int dot = prefix.rfind(".");
        scope_name = dot >= 0 ? prefix.substr(dot + 1) : prefix;
    }
    TSNode anchor = ts_node_is_null(class_def) ? body : class_def;
    Dictionary scope = make_member(StringName("class"), scope_name, anchor, prefix, script_path);
    scope[K.class_name] = String();
    scope[K.end_line]   = (int)ts_node_end_point(anchor).row;
    if (ts_node_is_null(class_def)) {
        scope[K.extends] = String();
    } else {
        TSNode ext = ts_field_node(class_def, "extends");
        scope[K.extends] = ts_node_is_null(ext) ? String() : extends_from_node(ext, src, src_len);
    }

    Dictionary constants = inherited_constants.duplicate();
    Dictionary this_scope_consts;
    uint32_t count = ts_node_named_child_count(body);

    // Pass 1: collect own consts/enums so they propagate to inner classes regardless of
    // declaration order (GDScript allows forward references to constants).
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        const char *t = ts_node_type(child);
        if (strcmp(t, "const_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            String type = ts_field(child, "type", src, src_len);
            TSNode val_node = ts_field_node(child, "value");
            Dictionary info = make_member(StringName("const"), name, child, prefix, script_path);
            info[K.type]            = to_string_name(type);
            info[K.has_static_type] = !type.is_empty();
            info[K.assignment]      = ts_node_is_null(val_node) ? String() : ts_text(val_node, src, src_len);
            constants[name] = info;
            this_scope_consts[name] = info;
        } else if (strcmp(t, "enum_definition") == 0) {
            String name = ts_field(child, "name", src, src_len);
            int line = (int)ts_node_start_point(child).row;
            String key = name.is_empty() ? ("@anon_enum_" + itos(line)) : name;
            Dictionary info = make_member(StringName("enum"), key, child, prefix, script_path);
            info[K.type]         = StringName();
            constants[key] = info;
            this_scope_consts[key] = info;
        }
    }

    // Build the constants dict that inner classes will inherit (includes ours + ancestors').
    Dictionary child_inherited = inherited_constants.duplicate();
    Array ck = this_scope_consts.keys();
    for (int ki = 0; ki < ck.size(); ki++)
        child_inherited[ck[ki]] = this_scope_consts[ck[ki]];

    // Pass 2: everything else.
    Dictionary members;
    Dictionary inner_classes;

    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        const char *t = ts_node_type(child);

        if (strcmp(t, "const_statement") == 0 || strcmp(t, "enum_definition") == 0) continue;

        if (ts_node_is_null(class_def)) {
            if (strcmp(t, "extends_statement") == 0) { scope[K.extends] = extends_from_node(child, src, src_len); continue; }
            if (strcmp(t, "class_name_statement") == 0) { scope[K.class_name] = ts_field(child, "name", src, src_len); continue; }
        }

        if (strcmp(t, "variable_statement") == 0 ||
            strcmp(t, "export_variable_statement") == 0 ||
            strcmp(t, "onready_variable_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            TSNode sf = ts_field_node(child, "static");
            TSNode type_node = ts_field_node(child, "type");
            String type;
            if (!ts_node_is_null(type_node) &&
                strcmp(ts_node_type(type_node), "inferred_type") != 0) {
                type = ts_text(type_node, src, src_len);
            }
            TSNode val_node = ts_field_node(child, "value");
            Dictionary info = make_member(ts_node_is_null(sf) ? StringName("var") : StringName("static var"),
                                          name, child, prefix, script_path);
            info[K.type]            = to_string_name(type);
            info[K.has_static_type] = !type.is_empty();
            info[K.assignment]      = ts_node_is_null(val_node) ? String() : ts_text(val_node, src, src_len);
            maybe_attach_lambda_v2(child, info, src, src_len, prefix, script_path);
            members[name] = info;
            continue;
        }

        if (strcmp(t, "signal_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            Dictionary info = make_member(StringName("signal"), name, child, prefix, script_path);
            info[K.type]         = StringName();
            info[K.args]         = parse_params_v2(ts_field_node(child, "parameters"), src, src_len, prefix, script_path);
            members[name] = info;
            continue;
        }

        if (strcmp(t, "function_definition") == 0 || strcmp(t, "constructor_definition") == 0) {
            bool is_ctor = strcmp(t, "constructor_definition") == 0;
            String name = is_ctor ? String("_init") : ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            Dictionary locals;
            TSNode body_node = ts_field_node(child, "body");
            if (!ts_node_is_null(body_node)) {
                uint32_t bc = ts_node_named_child_count(body_node);
                for (uint32_t j = 0; j < bc; j++)
                    collect_locals_v2(ts_node_named_child(body_node, j), src, src_len, prefix, script_path, locals);
            }
            Dictionary info = make_member(has_child_type(child, "static_keyword") ? StringName("static func") : StringName("func"),
                                          name, child, prefix, script_path);
            info[K.type]         = StringName();
            info[K.return_type]  = to_string_name(ts_field(child, "return_type", src, src_len));
            info[K.end_line]     = (int)ts_node_end_point(child).row;
            info[K.args]         = parse_params_v2(ts_field_node(child, "parameters"), src, src_len, prefix, script_path);
            info[K.locals]       = locals;
            members[name] = info;
            continue;
        }

        if (strcmp(t, "class_definition") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            String new_path = prefix.is_empty() ? name : (prefix + String(".") + name);
            TSNode ext = ts_field_node(child, "extends");
            Dictionary stub = make_member(StringName("class"), name, child, new_path, script_path);
            stub[K.extends]      = ts_node_is_null(ext) ? String() : extends_from_node(ext, src, src_len);
            stub[K.end_line]     = (int)ts_node_end_point(child).row;
            stub[K.type]         = to_string_name(new_path.is_empty() ? script_path : script_path + String(".") + new_path);
            inner_classes[name]  = stub;

            TSNode inner_body = ts_field_node(child, "body");
            if (!ts_node_is_null(inner_body))
                collect_script(inner_body, child, src, src_len, new_path, script_path, child_inherited, out);
            continue;
        }
    }

    scope[K.members]       = members;
    scope[K.constants]     = constants;
    scope[K.inner_classes] = inner_classes;
    out[prefix] = scope;
}

// ---------------------------------------------------------------------------
// GDScriptTreeParser
// ---------------------------------------------------------------------------

void GDScriptTreeParser::_bind_methods() {
    ClassDB::bind_method(D_METHOD("parse_script", "script_path"), &GDScriptTreeParser::parse_script);
    ClassDB::bind_method(D_METHOD("get_classes"),               &GDScriptTreeParser::get_classes);
    ClassDB::bind_method(D_METHOD("get_extends", "path"),       &GDScriptTreeParser::get_extends);
    ClassDB::bind_method(D_METHOD("get_members",       "path", "changed_only"), &GDScriptTreeParser::get_members,       DEFVAL(false));
    ClassDB::bind_method(D_METHOD("get_constants",     "path", "changed_only"), &GDScriptTreeParser::get_constants,     DEFVAL(false));
    ClassDB::bind_method(D_METHOD("get_inner_classes", "path", "changed_only"), &GDScriptTreeParser::get_inner_classes, DEFVAL(false));
}

Dictionary GDScriptTreeParser::get_classes() {
    Dictionary out;
    if (_edited || !_tree) return out;
    collect_classes(ts_tree_root_node(_tree), TSNode{}, _src.get_data(), _src_len,
                    String(), out);
    return out;
}

String GDScriptTreeParser::get_extends(const String &p_path) {
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

Dictionary GDScriptTreeParser::get_members(const String &p_path, bool p_changed_only) {
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

Dictionary GDScriptTreeParser::get_constants(const String &p_path, bool p_changed_only) {
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

Dictionary GDScriptTreeParser::get_inner_classes(const String &p_path, bool p_changed_only) {
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

Dictionary GDScriptTreeParser::parse_script(const String &p_script_path) {
    Dictionary out;
    if (_edited || !_tree) return out;
    Dictionary inherited_constants;
    collect_script(ts_tree_root_node(_tree), TSNode{}, _src.get_data(), _src_len,
                   String(), p_script_path, inherited_constants, out);
    return out;
}

} // namespace godot
