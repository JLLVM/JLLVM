// RUN: javac --add-exports java.base/jdk.internal.reflect=ALL-UNNAMED %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

import jdk.internal.reflect.Reflection;

import java.lang.reflect.Modifier;

class Test
{
    public static native void print(boolean b);
    public static native void print(int i);

    public final class C { }
    interface I { }

    public class Other
    {
        public static void foo()
        {
            Test.print(Reflection.getCallerClass() == Other.class);
        }
    }

    public static void main(String[] args)
    {
        // CHECK: 1
        print(Reflection.getCallerClass() == Test.class);
        // CHECK: 1
        Other.foo();

        // CHECK: 49
        // public final super (1 + 16 + 32)
        print(Reflection.getClassAccessFlags(C.class));
        // CHECK: 1536
        // abstract interface (512 + 1024)
        print(Reflection.getClassAccessFlags(I.class));
    }
}
