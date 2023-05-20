#!/usr/env python

import json, io, sys, os

basepath = os.path.dirname(sys.argv[1])

def process_tests(package, tests):
	if "$include" in tests:
		file = tests["$include"][0]
		path = tests["$include"][1:]
		json_file = json.loads(open(basepath + "/" + file).read())
		for p in path:
			json_file = json_file.get(p, {})
		process_tests(package, json_file)
	else:
		for t in tests:
			if "Static" not in tests[t].get("exclude", { }):
				if "expr" in tests[t]:
					sys.stdout.write("%s\n%s\nEval({ %s } nil);" % (package, t, tests[t]["expr"]))
				else:
					sys.stdout.write("%s\n%s\nTest:%s();" % (package, t, t))

master = json.loads(open(sys.argv[1]).read()).get("eval", {})

for package in master:
	process_tests(package, master[package])