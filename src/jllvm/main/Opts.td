include "llvm/Option/OptParser.td"

class F<string letter, string help> : Flag<["-"], letter>, HelpText<help>;

def classpath : Separate<["-"], "cp">, MetaVarName<"<class search path>">;
def : Separate<["-"], "classpath">, Alias<classpath>;
def : Separate<["--"], "class-path">, Alias<classpath>;

def Xenable_test_utils : F<"Xenable-test-utils", "Enables native test functions">;
