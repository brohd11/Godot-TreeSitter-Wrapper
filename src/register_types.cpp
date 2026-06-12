#include "register_types.h"
#include "tree_sitter_gd.h"
#include "gdscript_tree_parser.h"
#include "gdscript_tree_query.h"
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_tree_sitter_gd_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    ClassDB::register_class<GDScriptTreeSitter>();
    ClassDB::register_class<GDScriptTreeParser>();
    ClassDB::register_class<GDScriptTreeQuery>();
}

void uninitialize_tree_sitter_gd_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
}

extern "C" {
GDExtensionBool GDE_EXPORT tree_sitter_gd_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    const GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization
) {
    GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_tree_sitter_gd_module);
    init_obj.register_terminator(uninitialize_tree_sitter_gd_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
