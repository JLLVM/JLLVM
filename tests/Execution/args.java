// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class foo bar baz | FileCheck %s
// RUN: jllvm -Xint %t/Test.class foo bar baz | FileCheck %s

class Test
{
    public static native void print(String s);

    public static void main(String[] args)
    {
        for(String arg : args)
        {
            // CHECK: foo
            // CHECK-NEXT: bar
            // CHECK-NEXT: baz
            print(arg);
        }
    }
}
