#pragma once
#include "tree_sitter_gd.h"
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

// Single-pass full extraction of a GDScript file's structure.
//
// parse_script(script_path) walks the tree once and returns a dictionary keyed
// by access path ("" for the file root, "Outer.Inner" for nested classes). Each
// entry is a class scope:
//   { "member_type":"class", "member_name", "access_path", "script_path",
//     "class_name", "extends", "line_index", "column_index", "end_line",
//     "members":       { name → member info },
//     "constants":     { name → const/enum info (inherited consts included) },
//     "inner_classes": { name → stub } }
// member_type / member_name / type / return_type / access_path / script_path are
// StringName; positional fields are ints; assignment/default hold expression text.
//
// For granular per-path / changed_only queries, see GDScriptTreeQuery.

class GDScriptTreeParser : public GDScriptTreeSitter {
    GDCLASS(GDScriptTreeParser, GDScriptTreeSitter);

protected:
    static void _bind_methods();

public:
    Dictionary parse_script(const String &p_script_path);
};

} // namespace godot
