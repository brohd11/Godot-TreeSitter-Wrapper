class_name GDScriptEditorParser extends RefCounted

## Wraps GDScriptTreeParser with incremental parsing.
##
## parse_text() is the worker that reparses only when the CodeEdit's text version
## actually changed (cheap O(1) get_version() check, ~10us). It is wired to the
## text_changed signal AND meant to be called from a syntax highlighter's
## per-line _get_line_highlighting(), which Godot calls BEFORE text_changed fires.
## Whichever runs first reparses; the other no-ops on the matching version.
##
## Usage:
##   var ep := GDScriptEditorParser.new()
##   ep.attach(my_code_edit, "res://my_script.gd")
##   # in the highlighter's _get_line_highlighting(line):
##   if ep.parse_text():
##       _refresh_highlight_caches()   # only when a reparse happened
##   var data := ep.parse()

var parser := GDScriptTreeParser.new()
var _edit: CodeEdit = null
var _script_path := ""
var _prev_version := 0


func attach(edit: CodeEdit, script_path: String = "") -> void:
	if _edit != null and _edit.text_changed.is_connected(parse_text):
		_edit.text_changed.disconnect(parse_text)
	_edit = edit
	_script_path = script_path
	if not script_path.is_empty():
		parser.open_file(script_path)
	else:
		parser.open_text(edit.text)
	_prev_version = edit.get_version()
	edit.text_changed.connect(parse_text)


func detach() -> void:
	if _edit != null and _edit.text_changed.is_connected(parse_text):
		_edit.text_changed.disconnect(parse_text)
	_edit = null
	_prev_version = 0


func parse() -> Dictionary:
	return parser.parse_script(_script_path)


## True when the parse is up to date with the editor (or no editor attached).
## O(1): compares Godot's text version counter, which only bumps on text edits.
func cache_valid() -> bool:
	return _edit == null or _edit.get_version() == _prev_version


## Reparse incrementally if the buffer changed since the last sync; otherwise a
## cheap no-op. Returns true iff a reparse actually happened. Safe to call from
## both the text_changed signal and the highlighter's per-line path.
func parse_text() -> bool:
	if cache_valid():
		return false
	# update_text diffs against the previous source and reparses incrementally,
	# all in C++ (the byte diff is far too slow in GDScript).
	parser.update_text(_edit.text)
	_prev_version = _edit.get_version()
	return true
