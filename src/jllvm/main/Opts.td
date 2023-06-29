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
