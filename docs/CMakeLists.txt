# Copyright (C) 2023 The JLLVM Contributors.
#
# This file is part of JLLVM.
#
# JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 3, or (at your option) any later version.
#
# JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
# of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
# see <http://www.gnu.org/licenses/>.

find_program(SPHINX_EXECUTABLE NAMES sphinx-build REQUIRED)

add_custom_target(create-build-dir
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Copying docs sources")

add_custom_target(docs
        ${SPHINX_EXECUTABLE} -b html . build
        COMMENT "Building Sphinx docs"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(docs PROPERTIES ADDITIONAL_CLEAN_FILES "${CMAKE_CURRENT_BINARY_DIR}/build")
add_dependencies(docs create-build-dir)
