extends GDScriptCodeEditTreeSitter

# intended for use with GDScriptParser and SyntaxPlus Highlighter
# use as preloaded class

func _init():
	parser = GDScriptTreeParser.new()

func parse() -> Dictionary:
	return parser.parse_script(_script_path)

