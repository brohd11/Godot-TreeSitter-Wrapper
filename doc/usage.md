# Usage

The extension registers three `RefCounted` classes. `GDScriptTreeQuery` and
`GDScriptTreeParser` both **extend** `GDScriptTreeSitter`, so they inherit its full
parse lifecycle and add their own readers on top:

| Class | Purpose |
|-------|---------|
| `GDScriptTreeSitter` | Base. Owns the parser/tree, handles full + incremental parsing, and exposes raw tree-sitter `query()`. |
| `GDScriptTreeQuery` | Adds granular, per-path structural getters (members, constants, inner classes, …). |
| `GDScriptTreeParser` | One-shot full-tree extraction tailored to a specific workflow. See the [footnote](#gdscripttreeparser-footnote). |

All query/reader methods return empty values if called between `apply_edit` and
`reparse_text` (i.e. while the tree is marked edited but not yet reparsed).

---

## GDScriptTreeSitter (base)

### Parse lifecycle

| Method | Description |
|--------|-------------|
| `open_text(text: String)` | Full parse of a source string. |
| `open_file(path: String) -> bool` | Full parse from a file path. Returns `false` on error. |
| `update_text(new_text: String)` | **Recommended incremental path.** Diffs `new_text` against the previous source in C++ and reparses incrementally in one call. Falls back to a full parse if there's no prior tree. |
| `apply_edit(start_byte, old_end_byte, new_end_byte, start_row, start_col, old_end_row, old_end_col, new_end_row, new_end_col)` | Low-level alternative to `update_text`: mark a single contiguous edit by byte range, then call `reparse_text`. |
| `reparse_text(text: String)` | Incremental reparse following `apply_edit`. |

Most callers want `open_text`/`open_file` once, then `update_text` on every change —
it computes the byte diff for you. Use `apply_edit` + `reparse_text` only when you
already know the exact edited range and want to supply it yourself.

> **Column note:** `apply_edit` columns are UTF-8 *byte* offsets. Godot's `CodeEdit`
> reports *character* (code-point) columns. These are identical for ASCII but diverge on
> multibyte characters (accented letters, CJK, emoji). Safe as-is for ASCII-only source;
> add a byte-conversion step otherwise. `update_text` is unaffected — it diffs bytes
> internally.

### query()

`query(pattern: String) -> Array`

Runs a tree-sitter [S-expression query](https://tree-sitter.github.io/tree-sitter/using-parsers#query-syntax)
against the current tree. Returns an `Array` of `Dictionary`, one per match, each keyed
by capture name:

```gdscript
# match shape
{
  "<capture_name>": {
    "text":       String,
    "start_line": int,   # 0-indexed
    "end_line":   int,
    "start_col":  int,
    "end_col":    int,
  },
  # ... one entry per capture in the match
}
```

```gdscript
var ts := GDScriptTreeSitter.new()
ts.open_text(code)
for m in ts.query("(function_definition name: (name) @fn)"):
    print(m["fn"]["text"], " @ line ", m["fn"]["start_line"])
```

### Bracket mode

`set_bracket_mode(enabled: bool)` / `get_bracket_mode() -> bool` / `get_brackets() -> Dictionary` / `clear_brackets()`

Opt-in bracket tracking for colored-bracket highlighting. While enabled, every
parse maintains the bracket map internally: `update_text()` re-scans only the
rows touched by the edit (the byte diff union tree-sitter's changed ranges, so
structural fall-out like an unmatched quote restringing the lines below is
covered) and syncs the public Dictionary in place — the cost is paid at parse
time, not read time. A wholesale text replacement (e.g. swapping scripts on a
shared parse instance) triggers a full rescan instead, so no rows from the
previous document can survive. `clear_brackets()` drops the map without
disabling bracket mode (the next parse rebuilds it) — `detach()` on
`GDScriptCodeEditTreeSitter` calls it for you.

```gdscript
parser.set_bracket_mode(true)   # once, after creating the parser
parser.open_text(text)
var bracket_map: Dictionary = parser.get_brackets()
# { line: { column: depth } } — all ints, insertion-ordered;
# lines with no brackets have no entry
```

`get_brackets()` always returns the **same Dictionary object** (full scans
`clear()`+refill it), so you can fetch it once and hold the reference across
edits — it is already up-to-date after every `update_text()`. Treat it as
**read-only**: mutating it corrupts the maintenance bookkeeping.

- `column` is a *character* column (unlike the byte columns used by `query()`
  and `apply_edit()`).
- `depth` is the raw nesting level shared by an opener and its match; it is
  **not clamped at 0** — a stray closer pushes it negative until balanced.
  Color with `posmod(depth, num_colors)`.
- Brackets inside strings and comments never appear (their contents are not
  tokens in the grammar).

---

## GDScriptTreeQuery

Extends `GDScriptTreeSitter` (so it has the entire lifecycle above) and adds structural
getters. Every method takes an **access path**: `""` for the file root, `"Inner"` for a
top-level inner class, `"Outer.Inner"` for a nested one. `get_classes()` lists every valid
path.

The three `get_*` collection methods accept an optional `changed_only: bool = false`. When
`true`, only entries whose tree node was touched by the last incremental reparse are
returned (requires `apply_edit` + `reparse_text` — not the all-in-one `update_text`).

| Method | Returns |
|--------|---------|
| `get_classes() -> Dictionary` | One entry per reachable class scope, keyed by access path. |
| `get_extends(path: String) -> String` | Parent class for the given path (`""` for root). |
| `get_members(path, changed_only = false) -> Dictionary` | Variables, functions, signals, and `class_name` for the given path. |
| `get_constants(path, changed_only = false) -> Dictionary` | `const` and `enum` declarations. |
| `get_inner_classes(path, changed_only = false) -> Dictionary` | Direct inner `class` definitions. |

### Return shapes

**`get_classes()`** — keyed by access path:
```
# root ("")
{ "extends": String, "class_name": String, "line": int, "end_line": int }
# inner classes
{ "extends": String, "line": int, "end_line": int }
```

**`get_members()`** — keyed by member name. Shape depends on `keyword`:

```
# variable
{ "keyword": "var" | "static var", "line": int, "type": String, "changed": bool,
  # present only when the value is a lambda:
  "lambda": { …lambda shape… } }

# signal
{ "keyword": "signal", "line": int, "changed": bool,
  "args": { param_name: { "type": String, "default": String, "variadic": bool } } }

# function   (key is "_init" for a constructor)
{ "keyword": "func" | "static func", "line": int, "end_line": int,
  "return_type": String, "changed": bool,
  "args":   { param_name: { "type": String, "default": String, "variadic": bool } },
  "locals": { var_name:   { "keyword", "line", "type" [, "lambda": {…}] } } }
```

`line`/`end_line` are 0-indexed (matching `CodeEdit`). `type`/`return_type`/`default` are
`""` when absent. A `class_name` statement, if present, is stored under the special key
**`"__class_name__"`**: `{ "keyword": "class_name", "line": int, "name": String, "changed": bool }`.

**`get_constants()`** — keyed by name:
```
{ "keyword": "const", "line": int, "type": String, "changed": bool }   # const
{ "keyword": "enum",  "line": int,                  "changed": bool }   # enum (no type)
```
Anonymous enums use a generated key: `"@anon_enum_<line>"`.

**`get_inner_classes()`** — keyed by name:
```
{ "line": int, "extends": String, "changed": bool }
```

**Lambda shape** (recursive — appears in `var`/`local` entries whose value is a lambda):
```
{ "args": {…}, "return_type": String, "line": int, "end_line": int, "locals": {…} }
```

---

## Bundled GDScript helper

The addon ships `GDScriptCodeEditTreeSitter` (`gdscript_code_edit_tree_sitter.gd`), a thin
`RefCounted` wrapper that binds a `GDScriptTreeSitter` to a `CodeEdit` and reparses only
when the editor's text version actually changes — wire `attach(edit, path)` once and call
`parse_text()` from `text_changed` (and/or a highlighter's per-line callback). It exposes
`query()` straight through. See the file's header comment for the full pattern.

`path` is a label, not a source: it is what `parse_script()` stamps into the parsed member data as
`script_path`, and the text always comes from the attached `CodeEdit` so an unsaved buffer is still
reflected. Pass `prefer_code_edit = false` to parse the saved file (`open_file`) instead. Use
`set_script_path(path)` to re-label without touching the tree, e.g. when one `CodeEdit` is reused
across scripts.

---

## GDScriptTreeParser (footnote)

`GDScriptTreeParser` is a single-pass, full-tree extractor built for one specific
workflow (a type-resolution parser + scope-aware highlighter), so its output shape is
opinionated rather than general-purpose.

| Method | Returns |
|--------|---------|
| `parse_script(script_path: String) -> Dictionary` | Whole-file structure keyed by access path; each entry is a class scope with `members`, `constants`, `inner_classes`, and inheritance info. |
| `sparse_parse() -> Dictionary` | Lightweight per-keystroke pass for syntax highlighting: `{ "members": {…}, "lines": {…} }`, two parallel trees keyed by access path. |

The exact dictionary shapes are documented in the class header
(`src/gdscript_tree_parser.h`). The companion helper
`gdscript_code_edit_tree_parser.gd` swaps the parser into the bundled wrapper above and
adds a `parse()` convenience method.
