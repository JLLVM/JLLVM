#  Copyright (C) 2023 The JLLVM Contributors.
#
#  This file is part of JLLVM.
#
#  JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 3, or (at your option) any later version.
#
#  JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
#  of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
#  see <http://www.gnu.org/licenses/>.

import os
import platform
import re
import shutil
import subprocess
import tempfile

import lit.formats
import lit.util

from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = 'JLLVM'

config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files.
config.suffixes = ['.java', '.j']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.jllvm_obj_root, 'tests')

config.substitutions.append(('%PATH%', config.environment['PATH']))

llvm_config.with_system_environment(
    ['HOME', 'INCLUDE', 'LIB', 'TMP', 'TEMP', 'TSAN_OPTIONS', 'ASAN_OPTIONS',
     'UBSAN_OPTIONS'])

llvm_config.use_default_substitutions()

# excludes: A list of directories to exclude from the testsuite. The 'Inputs'
# subdirectories contain auxiliary inputs for various tests in their parent
# directories.
config.excludes = ['Inputs', 'CMakeLists.txt', 'jllvm-lit.py', 'lit.cfg.py']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.jllvm_obj_root, 'tests')

# Tweak the PATH to include the tools dir.
llvm_config.with_environment('PATH', config.llvm_tools_dir, append_path=True)

tool_dirs = [
    config.jllvm_tools_dir, config.llvm_tools_dir, config.java_home
]

tools = [
    ToolSubst('jllvm', extra_args=[
        '%{JLLVM_EXTRA_ARGS}'
    ]), 'jllvm-jvmc', 'javac', ToolSubst('jasmin', f'java -jar {config.jasmin_src}/jasmin.jar')
]

llvm_config.add_tool_substitutions(tools, tool_dirs)

# FileCheck -enable-var-scope is enabled by default in MLIR test
# This option avoids to accidentally reuse variable across -LABEL match,
# it can be explicitly opted-in by prefixing the variable name with $
config.environment['FILECHECK_OPTS'] = "-enable-var-scope --allow-unused-prefixes=false"

config.substitutions.append(('%{JLLVM_EXTRA_ARGS}', ''))
