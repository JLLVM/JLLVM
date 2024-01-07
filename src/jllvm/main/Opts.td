// Copyright (C) 2023 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

include "llvm/Option/OptParser.td"

class F<string letter, string help> : Flag<["-"], letter>, HelpText<help>;

def classpath : Separate<["-"], "cp">, MetaVarName<"<class search path>">;
def : Separate<["-"], "classpath">, Alias<classpath>;
def : Separate<["--"], "class-path">, Alias<classpath>;

def grp_internal : OptionGroup<"Internal"> {
    let Flags = [HelpHidden];
}

def Xdebug_EQ : Joined<["-"], "Xdebug=">, HelpText<"Enable debug debug logging">, Group<grp_internal>;
def Xdebug : F<"Xdebug", "Enable debug JVM debug logging">, Alias<Xdebug_EQ>, AliasArgs<["jvm"]>, Group<grp_internal>;
def Xno_system_init : F<"Xno-system-init", "Do not do System initialization (initPhase1 to 3)">, Group<grp_internal>;
def Xsystem_init : F<"Xsystem-init", "Do System initialization (initPhase1 to 3) (Default)">, Group<grp_internal>;

def Xenable_test_utils : F<"Xenable-test-utils", "Enables native test functions">, Group<grp_internal>;
def Xint : F<"Xint", "Execute code only with Interpreter">, Group<grp_internal>;
def Xjit : F<"Xjit", "Execute code only with JIT">, Group<grp_internal>;
def Xback_edge_threshold_EQ : Joined<["-"], "Xback-edge-threshold=">,
    HelpText<"Configure threshold for performing OSR on a backedge. Specify 0 to disable entirely.">,
    Group<grp_internal>, MetaVarName<"<count>">;
