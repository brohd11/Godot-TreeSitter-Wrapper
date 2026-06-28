#include "gdscript_tree_parser.h"
#include "ts_helpers.h"
#include "gd_keys.h"
#include <godot_cpp/core/class_db.hpp>
#include <tree_sitter/api.h>
#include <cstring>

namespace godot {

// ---------------------------------------------------------------------------
// parse_script() — single-pass full extraction
// ---------------------------------------------------------------------------

static StringName to_string_name(const String &s) {
    return s.is_empty() ? StringName() : StringName(s);
}

// Resolve a var/const `type` field.
//   explicit annotation (`: int`) -> type text, has_static = true
//   inferred (`:=`) or no annotation -> empty,   has_static = false
// tree-sitter does no inference; an `inferred_type` node is only the `:=` token.
static String extract_type(TSNode stmt, const char *src, uint32_t src_len,
                           bool &out_has_static) {
    out_has_static = false;
    TSNode type_node = ts_field_node(stmt, "type");
    if (ts_node_is_null(type_node)) return String();
    if (strcmp(ts_node_type(type_node), "inferred_type") == 0) return String();
    out_has_static = true;
    return ts_text(type_node, src, src_len);
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


// Forward declaration: collect_locals and parse_lambda_info are mutually recursive.
static void collect_locals(TSNode node, const char *src, uint32_t src_len,
                               const String &access_path, const String &script_path,
                               Dictionary &locals);

static Dictionary parse_params(TSNode params, const char *src, uint32_t src_len,
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

static Dictionary parse_lambda_info(TSNode lambda, const char *src, uint32_t src_len,
                                         const String &access_path, const String &script_path) {
    const Keys &K = Keys::get();
    Dictionary locals;
    TSNode body_node = ts_field_node(lambda, "body");
    if (!ts_node_is_null(body_node)) {
        uint32_t bc = ts_node_named_child_count(body_node);
        for (uint32_t j = 0; j < bc; j++)
            collect_locals(ts_node_named_child(body_node, j), src, src_len, access_path, script_path, locals);
    }
    Dictionary info;
    info[K.args]        = parse_params(ts_field_node(lambda, "parameters"), src, src_len, access_path, script_path);
    info[K.return_type] = to_string_name(ts_field(lambda, "return_type", src, src_len));
    info[K.line_index]  = (int)ts_node_start_point(lambda).row;
    info[K.end_line]    = (int)ts_node_end_point(lambda).row;
    info[K.locals]      = locals;
    return info;
}

static void maybe_attach_lambda(TSNode var_node, Dictionary &info,
                                    const char *src, uint32_t src_len,
                                    const String &access_path, const String &script_path) {
    TSNode val = ts_field_node(var_node, "value");
    if (!ts_node_is_null(val) && strcmp(ts_node_type(val), "lambda") == 0)
        info[Keys::get().lambda] = parse_lambda_info(val, src, src_len, access_path, script_path);
}

static void collect_locals(TSNode node, const char *src, uint32_t src_len,
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
            bool has_static;
            String type = extract_type(node, src, src_len, has_static);
            TSNode val_node = ts_field_node(node, "value");
            int line_idx = (int)ts_node_start_point(node).row;
            int col_idx = (int)ts_node_start_point(node).column;
            Dictionary info = make_member(ts_node_is_null(sf) ? StringName("var") : StringName("static var"),
                                          name, node, access_path, script_path);
            info[K.type]            = to_string_name(type);
            info[K.has_static_type] = has_static;
            info[K.assignment]      = ts_node_is_null(val_node) ? String() : ts_text(val_node, src, src_len);
            maybe_attach_lambda(node, info, src, src_len, access_path, script_path);
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
        collect_locals(ts_node_named_child(node, i), src, src_len, access_path, script_path, locals);
}

// Lightweight inner-class entry: identity + extends + range, no members. `path`
// is the class's own access path; used both for sibling stubs and a scope's self.
static Dictionary make_class_stub(TSNode class_node, const String &name, const String &path,
                                  const String &script_path, const char *src, uint32_t src_len) {
    const Keys &K = Keys::get();
    TSNode ext = ts_field_node(class_node, "extends");
    Dictionary stub = make_member(StringName("class"), name, class_node, path, script_path);
    stub[K.extends]  = ts_node_is_null(ext) ? String() : extends_from_node(ext, src, src_len);
    stub[K.end_line] = (int)ts_node_end_point(class_node).row;
    stub[K.type]     = to_string_name(script_path + String(".") + path);
    return stub;
}

static void collect_script(TSNode body, TSNode class_def,
                             const char *src, uint32_t src_len,
                             const String &prefix, const String &script_path,
                             const Dictionary &inherited_constants,
                             const Dictionary &inherited_inner_classes, Dictionary &out) {
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
    // Accessible inner classes: everything visible from the parent (ancestors +
    // siblings + the parent itself), accumulated down the tree. Own children and
    // self are layered on below so they shadow inherited same-name entries.
    Dictionary accessible = inherited_inner_classes.duplicate();
    uint32_t count = ts_node_named_child_count(body);

    // Pass 1: collect own consts/enums so they propagate to inner classes regardless of
    // declaration order (GDScript allows forward references to constants). Inner-class
    // stubs are also built here so every sibling exists before we recurse into any child.
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        const char *t = ts_node_type(child);
        if (strcmp(t, "const_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            bool has_static;
            String type = extract_type(child, src, src_len, has_static);
            TSNode val_node = ts_field_node(child, "value");
            Dictionary info = make_member(StringName("const"), name, child, prefix, script_path);
            info[K.type]            = to_string_name(type);
            info[K.has_static_type] = has_static;
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
        } else if (strcmp(t, "class_definition") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            String new_path = prefix.is_empty() ? name : (prefix + String(".") + name);
            // own child shadows inherited same-name entry
            accessible[name] = make_class_stub(child, name, new_path, script_path, src, src_len);
        }
    }

    // Self wins same-name collisions with a nested class (InnerClass.InnerClass
    // resolves to the enclosing class). Root (empty prefix) has no self entry.
    if (!prefix.is_empty())
        accessible[scope_name] = make_class_stub(class_def, scope_name, prefix, script_path, src, src_len);

    // Build the constants dict that inner classes will inherit (includes ours + ancestors').
    Dictionary child_inherited = inherited_constants.duplicate();
    Array ck = this_scope_consts.keys();
    for (int ki = 0; ki < ck.size(); ki++)
        child_inherited[ck[ki]] = this_scope_consts[ck[ki]];

    // Pass 2: everything else.
    Dictionary members;

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
            bool has_static;
            String type = extract_type(child, src, src_len, has_static);
            TSNode val_node = ts_field_node(child, "value");
            Dictionary info = make_member(ts_node_is_null(sf) ? StringName("var") : StringName("static var"),
                                          name, child, prefix, script_path);
            info[K.type]            = to_string_name(type);
            info[K.has_static_type] = has_static;
            info[K.assignment]      = ts_node_is_null(val_node) ? String() : ts_text(val_node, src, src_len);
            maybe_attach_lambda(child, info, src, src_len, prefix, script_path);
            members[name] = info;
            continue;
        }

        if (strcmp(t, "signal_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            Dictionary info = make_member(StringName("signal"), name, child, prefix, script_path);
            info[K.type]         = StringName();
            info[K.args]         = parse_params(ts_field_node(child, "parameters"), src, src_len, prefix, script_path);
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
                    collect_locals(ts_node_named_child(body_node, j), src, src_len, prefix, script_path, locals);
            }
            Dictionary info = make_member(has_child_type(child, "static_keyword") ? StringName("static func") : StringName("func"),
                                          name, child, prefix, script_path);
            info[K.type]         = StringName();
            info[K.return_type]  = to_string_name(ts_field(child, "return_type", src, src_len));
            info[K.end_line]     = (int)ts_node_end_point(child).row;
            info[K.args]         = parse_params(ts_field_node(child, "parameters"), src, src_len, prefix, script_path);
            info[K.locals]       = locals;
            members[name] = info;
            continue;
        }

        if (strcmp(t, "class_definition") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            String new_path = prefix.is_empty() ? name : (prefix + String(".") + name);
            // Stub already built into `accessible` in Pass 1. Pass the full accessible
            // set (incl. this scope's self) down so descendants can reference it.
            TSNode inner_body = ts_field_node(child, "body");
            if (!ts_node_is_null(inner_body))
                collect_script(inner_body, child, src, src_len, new_path, script_path,
                               child_inherited, accessible, out);
            continue;
        }
    }

    scope[K.members]       = members;
    scope[K.constants]     = constants;
    scope[K.inner_classes] = accessible;
    out[prefix] = scope;
}

// ---------------------------------------------------------------------------
// sparse_parse() — lightweight symbol pass for syntax highlighting
// ---------------------------------------------------------------------------

// Parameter names only (no types/defaults/positions).
static Array sparse_args(TSNode params, const char *src, uint32_t src_len) {
    Array out;
    if (ts_node_is_null(params)) return out;
    uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count; i++) {
        TSNode p = ts_node_named_child(params, i);
        const char *t = ts_node_type(p);
        String name;
        if (strcmp(t, "identifier") == 0) {
            name = ts_text(p, src, src_len);
        } else if (strcmp(t, "typed_parameter") == 0 ||
                   strcmp(t, "default_parameter") == 0 ||
                   strcmp(t, "typed_default_parameter") == 0) {
            name = first_ident_child(p, src, src_len);
        } else if (strcmp(t, "variadic_parameter") == 0) {
            uint32_t vc = ts_node_named_child_count(p);
            if (vc > 0) {
                TSNode inner = ts_node_named_child(p, 0);
                name = strcmp(ts_node_type(inner), "identifier") == 0
                           ? ts_text(inner, src, src_len)
                           : first_ident_child(inner, src, src_len);
            }
        } else { continue; }
        if (!name.is_empty()) out.push_back(to_string_name(name));
    }
    return out;
}

// Stripped sibling of collect_script. Populates two parallel trees keyed by
// access path: `members_out` holds position-independent symbol data (name-only
// members/constants + a functions map of arg names) so it can be hashed to detect
// real symbol changes; `lines_out` holds the class + function line ranges.
// No locals, assignments, lambdas, types, inner-class lists, or inheritance.
static void collect_symbols(TSNode body, TSNode class_def,
                              const char *src, uint32_t src_len,
                              const String &prefix,
                              Dictionary &members_out, Dictionary &lines_out) {
    const Keys &K = Keys::get();

    Array members;
    Array constants;
    Dictionary mem_funcs;   // name -> { args }            (no positions)
    Dictionary line_funcs;  // name -> { line_index, end_line }

    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode child = ts_node_named_child(body, i);
        const char *t = ts_node_type(child);

        if (strcmp(t, "variable_statement") == 0 ||
            strcmp(t, "export_variable_statement") == 0 ||
            strcmp(t, "onready_variable_statement") == 0 ||
            strcmp(t, "signal_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (!name.is_empty()) members.push_back(to_string_name(name));
            continue;
        }

        if (strcmp(t, "const_statement") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (!name.is_empty()) constants.push_back(to_string_name(name));
            continue;
        }

        if (strcmp(t, "enum_definition") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) name = "@anon_enum_" + itos((int)ts_node_start_point(child).row);
            constants.push_back(to_string_name(name));
            continue;
        }

        if (strcmp(t, "function_definition") == 0 || strcmp(t, "constructor_definition") == 0) {
            bool is_ctor = strcmp(t, "constructor_definition") == 0;
            String name = is_ctor ? String("_init") : ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            Dictionary mf;
            mf[K.args] = sparse_args(ts_field_node(child, "parameters"), src, src_len);
            mem_funcs[name] = mf;
            Dictionary lf;
            lf[K.line_index] = (int)ts_node_start_point(child).row;
            lf[K.end_line]   = (int)ts_node_end_point(child).row;
            line_funcs[name] = lf;
            continue;
        }

        if (strcmp(t, "class_definition") == 0) {
            String name = ts_field(child, "name", src, src_len);
            if (name.is_empty()) continue;
            TSNode inner_body = ts_field_node(child, "body");
            if (!ts_node_is_null(inner_body)) {
                String new_path = prefix.is_empty() ? name : (prefix + String(".") + name);
                collect_symbols(inner_body, child, src, src_len, new_path, members_out, lines_out);
            }
            continue;
        }
    }

    Dictionary mem_scope;
    mem_scope[K.members]   = members;
    mem_scope[K.constants] = constants;
    mem_scope[K.functions] = mem_funcs;
    members_out[prefix] = mem_scope;

    TSNode anchor = ts_node_is_null(class_def) ? body : class_def;
    Dictionary line_scope;
    line_scope[K.line_index] = (int)ts_node_start_point(anchor).row;
    line_scope[K.end_line]   = (int)ts_node_end_point(anchor).row;
    line_scope[K.functions]  = line_funcs;
    lines_out[prefix] = line_scope;
}

// ---------------------------------------------------------------------------
// GDScriptTreeParser
// ---------------------------------------------------------------------------

void GDScriptTreeParser::_bind_methods() {
    ClassDB::bind_method(D_METHOD("parse_script", "script_path"), &GDScriptTreeParser::parse_script);
    ClassDB::bind_method(D_METHOD("sparse_parse"),               &GDScriptTreeParser::sparse_parse);
}

Dictionary GDScriptTreeParser::parse_script(const String &p_script_path) {
    Dictionary out;
    if (_edited || !_tree) return out;
    Dictionary inherited_constants;
    Dictionary inherited_inner_classes;
    collect_script(ts_tree_root_node(_tree), TSNode{}, _src.get_data(), _src_len,
                   String(), p_script_path, inherited_constants, inherited_inner_classes, out);
    return out;
}

Dictionary GDScriptTreeParser::sparse_parse() {
    Dictionary out;
    if (_edited || !_tree) return out;
    Dictionary members_out, lines_out;
    collect_symbols(ts_tree_root_node(_tree), TSNode{}, _src.get_data(), _src_len,
                    String(), members_out, lines_out);
    out[Keys::get().members] = members_out;
    out[Keys::get().lines]   = lines_out;
    return out;
}

} // namespace godot
