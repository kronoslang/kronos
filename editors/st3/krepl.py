import sublime
import sublime_plugin
import subprocess
import json
import os
import cgi
from pprint import pprint

from threading import Thread

repl = None
rpc_map = {}
rpc_id = 1

print("starting krepl plugin")

def kronos_is_nil(obj):
	return isinstance(obj, dict) and not dict

builtin_types = {
	"#": "#{0}",
	"true": "#True",
	"fn": "fn",
	"Vec": "&lt;{1}&gt;",
	"Int64": "{0}",
	"tag": "#{0}",
	":VM:World:World": "Ok!"
}

def kronos_to_str(obj, omit_parens = False):
	if isinstance(obj, list):
		if not obj:
			return "()"

		if kronos_is_nil(obj[-1]):
			return "[" + " ".join([kronos_to_str(e) for e in obj[0:-1]]) + "]"
		else:
			if omit_parens:
				return " ".join([kronos_to_str(e) for e in obj])
			else:
				return "(" + " ".join([kronos_to_str(e) for e in obj]) + ")"
	elif isinstance(obj, dict):
		if not obj:
			return "nil"
		for ty, val in obj.items():
			if ty in builtin_types:
				return builtin_types[ty].format(val, kronos_to_str(val, True))
			else:
				return ty[1:] + "{" + kronos_to_str(val, True) + "}"

	elif isinstance(obj, str):
		return json.dumps(obj)
	else:
		return str(obj)

def clean_phantoms(w):
	for v in w.views():
		v.erase_phantoms("krepl")

def plugin_unloaded():
	global repl
	if repl is not None:
		print("Terminating REPL process")
		repl.kill()
		repl = None


def format_phantom(html_content):
	return '<div style="background-color:rgba(0,0,0,0.8);padding-left:4px;padding-right:4px;">{0}</div>'.format(html_content)

def show_phantom(content, handler, pid, view, position, region, code):
	print("updating {}".format(content))
	view.erase_phantom_by_id(pid)
	return view.add_phantom("krepl", position, (format_phantom(content)), sublime.LAYOUT_INLINE, handler)

def readln(stream):
	line = ""
	while True:
		rd = stream.readline()

		if not rd:
			return line

		line += str(rd, encoding="utf-8");

		if "\n" in line:
			return line

		print("partial read: " + str(rd))

def rpc_send(stream, rpcid, method, params = {}):
	rpc_object = {
		"id": rpcid,
		"jsonrpc": "2.0",
		"method": method,
		"params": params
	}

	rpc_str = json.dumps(rpc_object).encode("utf-8")
	header = "Content-Length: {}\r\n\r\n".format(len(rpc_str)).encode("utf-8")

	print(rpc_str)

	stream.write(header + rpc_str)
	stream.flush()

def stop_instance(rpc_stream, context):
	global rpc_id
	context["view"].erase_phantom_by_id(context["pid"])
	rpc_send(rpc_stream, rpc_id, "stop", { "instance": context["instance"] })
	rpc_id += 1


def rpc_listener(repl, stream, console_print, on_error):
	global rpc_map
	try:
		with stream:
			print("Starting RPC listener")
			while True:
				header = readln(stream)
				empty = readln(stream)
				content = readln(stream)
				print("Received:" + content)
				msg = json.loads(content)

				if "id" in msg:
					if msg["id"] in rpc_map:
						context = rpc_map[msg["id"]]
						if "result" in msg:
							if "instance" in msg["result"]:
								context["pid"] = show_phantom("<a href='stop'>stop</a>",
															  (lambda x: stop_instance(repl.stdin, context)),
															  **context)
								context["instance"] = msg["result"]["instance"]
							else:
								context["pid"] = show_phantom(" ".join([kronos_to_str(val) for val in msg["result"]]),
															  None, **context)

				else:
					if msg["method"] == "message-bundle":
						for pipe, msgs in msg["params"].items():
							console_print(pipe, msgs)
					elif msg["method"] == "error":
							ed = msg["params"]
							console_print("err", [ed["message"],"\n"])
							if "log" in ed:
								console_print("err", [ed["log"],"\n"])

		print("repl listener terminated")
	except Exception as e:
		on_error(e)

def rpc_relay(stream, on_line):
	with stream:
		print("Starting RPC relay")
		while True:
			ln = readln(stream)
			if ln:
				on_line(ln)
			else:
				break


def init_repl(printfn, on_error):
	prefs = sublime.load_settings("Kronos.sublime-settings")
	krpc_path = prefs.get("kronos.krpc_path")
	lib_repo = prefs.get("kronos.core_library_repository")
	lib_version = prefs.get("kronos.core_library_version")

	print("Launching  <{}>\nLibrary override <[{} {}]>".format(krpc_path, lib_repo, lib_version))

	params = {
		"stdin": subprocess.PIPE,
		"stdout": subprocess.PIPE,
		"stderr": subprocess.PIPE,
	}

	if lib_repo or lib_version:
		env = os.environ.copy()
		if lib_repo:
			env["KRONOS_CORE_REPOSITORY"] = lib_repo
		if lib_version:
			env["KRONOS_CORE_REPOSITORY_VERSION"] = lib_version
		params["env"] = env

	if os.name == "nt":
		startupinfo = subprocess.STARTUPINFO()
		startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
		params["startupinfo"] = startupinfo

	pprint(params)
	proc = subprocess.Popen([krpc_path], **params)
	pprint(vars(proc))

	Thread(target = rpc_listener, args = (proc, proc.stdout, printfn or print, on_error,)).start()
	Thread(target = rpc_relay, args = (proc.stderr,print,)).start()


	return proc


class KreplPanicCommand(sublime_plugin.WindowCommand):
	def run(self):
		global repl
		global rpc_id
		if repl:
			rpc_send(repl.stdin, rpc_id, "panic")
			for v in self.window.views():
				v.erase_phantoms("krepl")
			rpc_id += 1

def remove_phantoms(av, ln):
	remove_entries = []

	for msgid, record in rpc_map.items():
		if record["view"] == av:
			pid = record["pid"]
			phregion = av.query_phantom(pid)
			if phregion:
				if ln.contains(phregion[0]):
					av.erase_phantom_by_id(pid)
					if "instance" in record:
						stop_instance(repl.stdin, record)

					remove_entries.append(msgid)

	for r in remove_entries:
		print("removing {}".format(r))
		del rpc_map[r]


def repl_error(err, w):
	global repl
	print(err)
	repl = None


class KreplCommand(sublime_plugin.WindowCommand):
	def __init__(self, param):
		super().__init__(param)
		self.console_panel = self.window.create_output_panel("krepl")
		self.console_panel.set_read_only(True)
		clean_phantoms(self.window)

	def to_out(self, pipe, msgs):
		self.window.run_command("show_panel", { "panel": "output.krepl"})
		self.console_panel.run_command("append",{
			"characters": "".join(msgs),
			"force": True,
			"scroll_to_end": True
		})

	def error(self, error):
		repl_error(error, self.window)

	def run(self):
		global repl
		global rpc_map
		global rpc_id

		av = self.window.active_view()

		if not repl:
			repl = init_repl(self.to_out, self.error)

		for sel in av.sel():
			if sel.empty():
				ln = av.full_line(sel)
			else:
				ln = sel

			phantom_pos = av.lines(ln)[-1]
			placeholder_pos = sublime.Region(phantom_pos.end(), phantom_pos.end())

			remove_phantoms(av, ln)

			pid = av.add_phantom("krepl", placeholder_pos, format_phantom('«'), sublime.LAYOUT_INLINE, print)

			rpc_map[rpc_id] = {
				"position": placeholder_pos,
				"view": av,
				"region": phantom_pos,
				"code": av.substr(ln),
				"pid": pid
			}

			rpc_send(repl.stdin, rpc_id, "evaluate", {
				"code": av.substr(ln),
				"invalidate": False
			})

			rpc_id += 1

class KreplInstanceCommand(sublime_plugin.WindowCommand):
	def __init__(self, param):
		super().__init__(param)
		self.console_panel = self.window.create_output_panel("krepl")
		self.console_panel.set_read_only(True)
		clean_phantoms(self.window)

	def to_out(self, pipe, msgs):
		self.window.run_command("show_panel", { "panel": "output.krepl"})
		self.console_panel.run_command("append",{
			"characters": "".join(msgs),
			"force": True,
			"scroll_to_end": True
		})

	def error(self, error):
		repl_error(error, self.window)

	def run(self):
		global repl
		global rpc_id

		av = self.window.active_view()

		if not repl:
			repl = init_repl(self.to_out, self.error)

		for sel in av.sel():
			if sel.empty():
				ln = av.full_line(sel)
			else:
				ln = sel

			phantom_pos = av.lines(ln)[-1]
			placeholder_pos = sublime.Region(phantom_pos.end(), phantom_pos.end())

			remove_phantoms(av, ln)

			pid = av.add_phantom("krepl", placeholder_pos, format_phantom('«'), sublime.LAYOUT_INLINE, print)
			code = "{{{}}}".format(av.substr(ln))

			rpc_map[rpc_id] = {
				"position": placeholder_pos,
				"view": av,
				"region": phantom_pos,
				"code": code,
				"pid": pid
			}

			rpc_send(repl.stdin, rpc_id, "start", {
				"code": code
			})

			rpc_id += 1

class KreplRestartCommand(sublime_plugin.WindowCommand):
	def run(self):
		global repl
		global rpc_id
		if repl:
			rpc_id = 1
			repl.kill()
			repl = None
			for v in self.window.views():
				v.erase_phantoms("krepl")
