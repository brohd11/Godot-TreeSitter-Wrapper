class_name GDScriptEditorParser extends RefCounted

## Wraps GDScriptTreeParser with incremental parsing driven by CodeEdit's text_changed signal.
##
## Usage:
##   var ep := GDScriptEditorParser.new()
##   ep.attach(my_code_edit, "res://my_script.gd")
##   var data := ep.parse()

var parser := GDScriptTreeParser.new()
var _edit: CodeEdit = null
var _script_path := ""
var _prev_bytes := PackedByteArray()


func attach(edit: CodeEdit, script_path: String = "") -> void:
	if _edit != null and _edit.text_changed.is_connected(_on_text_changed):
		_edit.text_changed.disconnect(_on_text_changed)
	_edit = edit
	_script_path = script_path
	if not script_path.is_empty():
		parser.open_file(script_path)
	else:
		parser.open_text(edit.text)
	_prev_bytes = edit.text.to_utf8_buffer()
	edit.text_changed.connect(_on_text_changed)


func detach() -> void:
	if _edit != null and _edit.text_changed.is_connected(_on_text_changed):
		_edit.text_changed.disconnect(_on_text_changed)
	_edit = null
	_prev_bytes = PackedByteArray()


func parse() -> Dictionary:
	return parser.parse_script(_script_path)


func _on_text_changed() -> void:
	var new_text: String = _edit.text
	var new_bytes: PackedByteArray = new_text.to_utf8_buffer()

	var plen := _prev_bytes.size()
	var nlen := new_bytes.size()

	# Scan forward to find the first differing byte.
	var sb := 0
	while sb < plen and sb < nlen and _prev_bytes[sb] == new_bytes[sb]:
		sb += 1

	# Scan backward to find old and new end bytes.
	var oeb := plen
	var neb := nlen
	while oeb > sb and neb > sb and _prev_bytes[oeb - 1] == new_bytes[neb - 1]:
		oeb -= 1
		neb -= 1

	# Convert byte positions to (row, col) where col is a byte offset within the line,
	# matching tree-sitter's TSPoint.column convention.
	var src_rc  := _byte_to_rc(_prev_bytes, sb)
	var oend_rc := _byte_to_rc(_prev_bytes, oeb)
	var nend_rc := _byte_to_rc(new_bytes,   neb)

	parser.apply_edit(
		sb,        oeb,        neb,
		src_rc.x,  src_rc.y,
		oend_rc.x, oend_rc.y,
		nend_rc.x, nend_rc.y,
	)
	parser.reparse_text(new_text)
	_prev_bytes = new_bytes


static func _byte_to_rc(bytes: PackedByteArray, pos: int) -> Vector2i:
	var row := 0
	var col := 0
	for i: int in pos:
		if bytes[i] == 0x0A:  # '\n'
			row += 1
			col = 0
		else:
			col += 1
	return Vector2i(row, col)
