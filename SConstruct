#!/usr/bin/env python
# GDExtension build script for gdscript_tree_parser
#
# Prerequisites (add as git submodules or clone manually):
#
#   git submodule add https://github.com/godotengine/godot-cpp.git godot-cpp
#   git submodule add https://github.com/tree-sitter/tree-sitter.git thirdparty/tree-sitter
#   git submodule add https://github.com/PrestonKnopp/tree-sitter-gdscript.git thirdparty/tree-sitter-gdscript
#   git submodule update --init --recursive
#
# Build (from this directory):
#   scons target=template_debug    # debug build
#   scons target=template_release  # release build
#
# The compiled library lands in bin/. Copy both bin/ and
# gdscript_tree_parser.gdextension into your Godot project.

import os

# ---- Sanity checks ----------------------------------------------------------

for dep, url in [
    ("godot-cpp",                          "https://github.com/godotengine/godot-cpp.git"),
    ("thirdparty/tree-sitter",             "https://github.com/tree-sitter/tree-sitter.git"),
    ("thirdparty/tree-sitter-gdscript",    "https://github.com/PrestonKnopp/tree-sitter-gdscript.git"),
]:
    if not os.path.isdir(dep):
        print("ERROR: {} not found. Clone it:\n  git submodule add {} {}".format(dep, url, dep))
        Exit(1)

ts_parser_c = "thirdparty/tree-sitter-gdscript/src/parser.c"
if not os.path.isfile(ts_parser_c):
    print("ERROR: {} not found. Did the tree-sitter-gdscript submodule init?".format(ts_parser_c))
    Exit(1)

# ---- Inherit godot-cpp environment ------------------------------------------

env = SConscript("godot-cpp/SConstruct")

# ---- Include paths ----------------------------------------------------------

env.Append(CPPPATH=[
    "src/",
    "thirdparty/tree-sitter/lib/include",
    # tree-sitter lib.c needs its own src dir on the include path
    "thirdparty/tree-sitter/lib/src",
])

# ---- Sources ----------------------------------------------------------------

# tree-sitter runtime (single amalgamated source)
ts_runtime = ["thirdparty/tree-sitter/lib/src/lib.c"]

# GDScript grammar (always has parser.c; scanner.c is confirmed present)
ts_grammar = ["thirdparty/tree-sitter-gdscript/src/parser.c"]

for scanner in [
    "thirdparty/tree-sitter-gdscript/src/scanner.c",
    "thirdparty/tree-sitter-gdscript/src/scanner.cc",
]:
    if os.path.isfile(scanner):
        ts_grammar.append(scanner)

ext_sources = [
    "src/register_types.cpp",
    "src/gdscript_tree_parser.cpp",
]

sources = ts_runtime + ts_grammar + ext_sources

# ---- Output -----------------------------------------------------------------

suffix = env["suffix"]      # e.g.  .macos.template_debug.arm64
shlibsuffix = env["SHLIBSUFFIX"]  # .dylib / .dll / .so

library = env.SharedLibrary(
    "bin/libgdscript_tree_parser{}{}".format(suffix, shlibsuffix),
    source=sources,
)

Default(library)
