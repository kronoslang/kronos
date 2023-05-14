#!/bin/bash

hg log --rev 'reverse(p2():ancestor(p1(),p2())-ancestor(p1(),p2()))' --template '- {desc}\n' | sed '/^merge/d' > message.txt
hg commit -el message.txt
