// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static int i = 5;

    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 5
        print(Test.i);
        Test.i = 3;
        // CHECK: 3
        print(Test.i);
    }
}
