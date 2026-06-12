#pragma once
#include "tree_sitter_gd.h"
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

// GDScript-specific structural queries on top of GDScriptTreeSitter.
//
// All methods accept an access path: "" for the file root, "Inner" for a top-level
// inner class, "Outer.Inner" for a nested one. get_classes() lists all valid paths.
//
// All three query methods accept an optional changed_only: bool = false.
// When true, only entries whose tree node was touched by the last incremental
// reparse are returned (requires apply_edit + reparse_text, not just reparse_text).
//
// get_members()  { name: { "keyword", "line", "type", "changed"
//                          funcs also: "end_line", "return_type",
//                            "args":   { param → { "type","default","variadic" } },
//                            "locals": { name  → { "keyword","line","type"[,"lambda"] } }
//                          vars whose value is a lambda also: "lambda": { same shape as func } } }
// get_constants(){ name: { "keyword", "line", "type", "changed" } }
// get_inner_classes() { name: { "line", "extends", "changed" } }

class GDScriptTreeParser : public GDScriptTreeSitter {
    GDCLASS(GDScriptTreeParser, GDScriptTreeSitter);

protected:
    static void _bind_methods();

public:
    Dictionary parse_script(const String &p_script_path);
    Dictionary get_classes();
    String     get_extends(const String &p_path);
    Dictionary get_members(const String &p_path, bool p_changed_only = false);
    Dictionary get_constants(const String &p_path, bool p_changed_only = false);
    Dictionary get_inner_classes(const String &p_path, bool p_changed_only = false);
};

} // namespace godot
