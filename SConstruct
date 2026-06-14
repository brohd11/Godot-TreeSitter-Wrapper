#!/usr/bin/env python
# GDExtension build script for tree-sitter-gd
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
# The compiled library lands in bin/. Run ./package.sh to assemble a
# distributable addon under build/tree-sitter-gd-<version>/ (the addon source
# lives in tree_sitter_gd/), or ./build-cross.sh to build all platforms first.

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
    "src/tree_sitter_gd.cpp",
    "src/gdscript_tree_parser.cpp",
    "src/gdscript_tree_query.cpp",
]

# Build the vendored tree-sitter runtime + grammar with optimization even when
# the rest of the build is unoptimized: it's generated/third-party C we never
# step through, and the large parser.c is painfully slow at -O0. Our own src/
# objects are left on the godot-cpp target defaults so they stay debuggable.
#
# Only bump when the build's own optimize level is below -O2 (dev_build ->
# optimize=none -> -O0, or optimize=debug -> -Og/-Od). For speed_trace (-O2),
# speed (-O3), size, and custom we inherit the build's flags unchanged so the
# libs are never downgraded. The appended flag lands after godot-cpp's own
# optimize flag, so it wins (last -O level on the command line).
ts_env = env.Clone()
if env["optimize"] in ("none", "debug"):
    ts_env.Append(CCFLAGS=(["/O2"] if env.get("is_msvc", False) else ["-O2"]))
ts_objects = [ts_env.SharedObject(s) for s in ts_runtime + ts_grammar]

sources = ts_objects + ext_sources

# ---- Output -----------------------------------------------------------------

suffix = env["suffix"]      # e.g.  .macos.template_debug.arm64
shlibsuffix = env["SHLIBSUFFIX"]  # .dylib / .dll / .so

library = env.SharedLibrary(
    "bin/libtree_sitter_gd{}{}".format(suffix, shlibsuffix),
    source=sources,
)

Default(library)
