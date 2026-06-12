#pragma once
#include <godot_cpp/variant/string_name.hpp>

namespace godot {

// ---------------------------------------------------------------------------
// Dictionary keys, interned as StringName once and reused for every entry.
// Shared by GDScriptTreeParser (parse_script) and GDScriptTreeQuery (get_*).
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

} // namespace godot
