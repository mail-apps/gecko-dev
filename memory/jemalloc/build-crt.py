#!/usr/bin/env python

import os, sys
from subprocess import Popen, STDOUT, PIPE

if 'MAKEFLAGS' in os.environ:
  del os.environ['MAKEFLAGS']
os.chdir(sys.argv[1])
proc = Popen(['nmake', 'dll_', 'mt', 'RETAIL_DLL_NAME=mozcrt19', 'RETAIL_LIB_NAME=msvcrt'], stdout=PIPE, stderr=STDOUT,
             cwd=sys.argv[1])

while True:
  line = proc.stdout.readline()
  if line == '':
    break
  line = line.rstrip()
  # explicitly ignore this fatal-sounding non-fatal error
  if line == "NMAKE : fatal error U1052: file 'makefile.sub' not found" or line == "Stop.":
    continue
  print line
sys.exit(proc.wait())
