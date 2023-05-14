#!/usr/env python

import json, io, sys

tests = json.loads(open(sys.argv[1]).read())

for module in sorted(set(tests["eval"].keys() + tests["audio"].keys())):
	sys.stdout.write("%s;" % module)