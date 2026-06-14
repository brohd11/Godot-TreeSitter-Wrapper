# Godot-TreeSitter-Wrapper

A Godot 4 GDExtension that exposes a RefCounted TreeSitter class. Uses:
[tree-sitter](https://tree-sitter.github.io) and the
[tree-sitter-gdscript](https://github.com/PrestonKnopp/tree-sitter-gdscript) grammar.

Simple operation, give it text or a file path, and it parses. Give it an updated text
it will calculate the byte offset and do an incremental parse.

There are a couple other classes here that extend the basic TreeSitter class, they demonstrate
where this can be useful. I use them in the following for faster parsing:
1. [GDScriptParser](https://github.com/brohd11/Godot-Addon-Lib/tree/main/alib_runtime/utils/gdscript/parser) - Type resolution parser
2. [SyntaxPlus](https://github.com/brohd11/Godot-GD-Syntax-Plus) - Scope aware syntax highlighter

For info on [usage](doc/usage.md)

---

## License

MIT — see [LICENSE](LICENSE).
*(tree-sitter: MIT, tree-sitter-gdscript: MIT, godot-cpp: MIT)*
