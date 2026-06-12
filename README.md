# tree-sitter-gd

A Godot 4 GDExtension that exposes a fast, incremental GDScript parser built on
[tree-sitter](https://tree-sitter.github.io) and the
[tree-sitter-gdscript](https://github.com/PrestonKnopp/tree-sitter-gdscript) grammar.

The core class `GDScriptTreeParser` (`RefCounted`) keeps the parse tree in native memory
and lets you query it on demand — no serialisation overhead on every keystroke.
Incremental reparsing (after a text edit) takes ~100 µs on a 1500-line file;
a full initial parse takes ~2–3 ms.

---

## Install (prebuilt)

1. Download the latest release zip from the [Releases](../../releases) page.
2. Unzip it into your Godot project root. It will create:
   ```
   addons/tree_sitter_gd/
   ├── tree_sitter_gd.gdextension
   ├── plugin.cfg
   ├── plugin.gd
   └── bin/   ← platform libraries
   ```
3. In Godot: **Project → Project Settings → Plugins** → enable **tree-sitter-gd**.

---

## API

All methods return empty values (`{}`, `[]`, `""`) if called before `open_text` / `open_file`,
or between `apply_edit` and `reparse_text`.

### Open / reparse

| Method | Description |
|--------|-------------|
| `open_text(text: String)` | Full parse of a source string. |
| `open_file(path: String) -> bool` | Full parse from a file path. Returns `false` on error. |
| `apply_edit(start_byte, old_end_byte, new_end_byte, start_row, start_col, old_end_row, old_end_col, new_end_row, new_end_col)` | Mark a single contiguous edit (byte offsets). Must be followed by `reparse_text`. |
| `reparse_text(text: String)` | Incremental reparse after `apply_edit`. |

> **Column note:** `apply_edit` columns are UTF-8 *byte* offsets. Godot's `CodeEdit`
> gives *character* (code-point) columns. These are identical for ASCII but diverge on
> multibyte characters (accented letters, CJK, emoji). Safe to use as-is for ASCII-only
> source; add a byte-conversion step for non-ASCII.

### Queries

All queries accept an optional `changed_only: bool = false`. When `true`, only members
whose tree node was affected by the most recent incremental reparse are returned.

| Method | Returns |
|--------|---------|
| `get_class_paths() -> Array` | `["", "Inner", "Outer.Inner", …]` — all reachable access paths. `""` is the file root. |
| `get_extends(path: String) -> String` | Parent class for the given path (`""` for root). |
| `get_members(path: String, changed_only: bool = false) -> Dictionary` | Variables, functions, signals, and `class_name` for the given path. |
| `get_constants(path: String, changed_only: bool = false) -> Dictionary` | `const` and `enum` declarations. |
| `get_inner_classes(path: String, changed_only: bool = false) -> Dictionary` | Direct inner `class` definitions with name, line, extends. |

### Member dict shapes

**Variable / signal / class_name entry:**
```
{
  "keyword":  "var" | "static var" | "signal" | "class_name",
  "line":     int,          # 0-indexed (matches CodeEdit)
  "type":     String,       # "" if untyped
  "changed":  bool,         # true if touched by last incremental reparse
  # signals also have:
  "args":     { param_name: { "type", "default", "variadic" } },
  # vars whose value is a lambda also have:
  "lambda":   { "args", "return_type", "line", "end_line", "locals": { … recursive } },
}
```

**Function entry:**
```
{
  "keyword":     "func" | "static func",
  "line":        int,
  "end_line":    int,
  "return_type": String,
  "args":        { param_name: { "type", "default", "variadic" } },
  "locals":      { var_name: { "keyword", "line", "type" [, "lambda": {…}] } },
  "changed":     bool,
}
```

**Constant / enum entry:**
```
{ "keyword": "const" | "enum", "line": int, "type": String, "changed": bool }
```
Anonymous enums use a generated key: `"@anon_enum_<line>"`.

**Inner class entry:**
```
{ "line": int, "extends": String, "changed": bool }
```

**Special key:** `"__class_name__"` — holds the `class_name` statement if present.

---

## Build from source

**Prerequisites (all platforms):** Git, Python 3, SCons (`pip install scons`).

### macOS

```bash
xcode-select --install    # one-time: installs clang
./setup.sh
scons target=template_debug
scons target=template_release
```

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential python3-pip git
pip3 install scons
./setup.sh
scons target=template_debug
scons target=template_release
```

Output: `bin/libtree_sitter_gd.linux.template_debug.x86_64.so`

### Windows (MSVC — recommended)

1. Download **Build Tools for Visual Studio** (free) from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) and select **"Desktop development with C++"**.
2. Install **Python 3** from [python.org](https://www.python.org/downloads/) — check "Add to PATH".
3. `pip install scons`
4. Open the **"x64 Native Tools Command Prompt for VS 20xx"** shortcut from the Start menu.
5. ```bat
   git clone <this-repo>
   cd tree-sitter-gd
   setup.bat
   scons target=template_debug
   scons target=template_release
   ```

Output: `bin/libtree_sitter_gd.windows.template_debug.x86_64.dll`

> **MinGW alternative:** Install MSYS2, then `pacman -S mingw-w64-x86_64-gcc python-pip`.
> Add `use_mingw=yes` to the scons command.

---

## Releasing prebuilt binaries

Build on each platform, then create a GitHub Release and upload a zip containing:
```
addons/tree_sitter_gd/
├── tree_sitter_gd.gdextension
├── plugin.cfg
├── plugin.gd
└── bin/
    ├── libtree_sitter_gd.macos.template_debug.universal.dylib
    ├── libtree_sitter_gd.macos.template_release.universal.dylib
    ├── libtree_sitter_gd.linux.template_debug.x86_64.so
    ├── libtree_sitter_gd.linux.template_release.x86_64.so
    ├── libtree_sitter_gd.windows.template_debug.x86_64.dll
    └── libtree_sitter_gd.windows.template_release.x86_64.dll
```

---

## License

MIT — see [LICENSE](LICENSE).
*(tree-sitter: MIT, tree-sitter-gdscript: MIT, godot-cpp: MIT)*
