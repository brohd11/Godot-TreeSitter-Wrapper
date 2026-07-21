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
//     "inner_classes": { name → stub } — every class referenceable by name from
//        this scope: inherited ancestors + siblings + own children + self
//        (root has no self). Own children shadow inherited same-name entries;
//        self shadows a nested same-name child (InnerClass.InnerClass → self). }
// member_type / member_name / type / return_type / access_path / script_path are
// StringName; positional fields are ints; assignment/default hold expression text.
//
// For granular per-path / changed_only queries, see GDScriptTreeQuery.
//
// sparse_parse() is a lightweight per-keystroke pass for syntax highlighting.
// Returns { "members": {...}, "lines": {...} }, two parallel trees keyed by access
// path (inner classes appear as their own entries; no nesting/inheritance):
//   members[path] = { members:[names], constants:[names], functions:{ name:{args:[names]} } }
//     -- position-independent; hash it to detect real symbol changes
//   lines[path]   = { line_index, end_line, functions:{ name:{line_index, end_line} } }
//     -- class + function ranges; refresh boundaries when only these moved
// Skips locals, assignments, lambdas, types, inner-class lists, and inheritance.
// (For colored-bracket highlighting see bracket mode on GDScriptTreeSitter.)

class GDScriptTreeParser : public GDScriptTreeSitter {
    GDCLASS(GDScriptTreeParser, GDScriptTreeSitter);

protected:
    static void _bind_methods();

public:
    Dictionary parse_script(const String &p_script_path);
    Dictionary sparse_parse();
};

} // namespace godot
