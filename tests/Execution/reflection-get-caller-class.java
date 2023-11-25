// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac --add-exports java.base/jdk.internal.reflect=ALL-UNNAMED %t/Other.java -d %t
// RUN: jllvm -Xjit %t/Other.class | FileCheck %s
// RUN: jllvm -Xint %t/Other.class | FileCheck %s

//--- Test.java

import jdk.internal.reflect.Reflection;

public class Test
{
    public static native void print(boolean b);

    public static void foo()
    {
        Test.print(Reflection.getCallerClass() == Test.class);
    }
}

//--- Other.java

import jdk.internal.reflect.Reflection;

public class Other
{
    public static void main(String[] args)
    {
        // CHECK: 1
        Test.print(Reflection.getCallerClass() == Other.class);
        // CHECK: 1
        Test.foo();
    }
}
