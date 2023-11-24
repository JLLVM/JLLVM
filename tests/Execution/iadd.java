// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 3;
        int y = 2;
        int z = 1;

        // TODO: workaround until the interpreter supports 'invokestatic' to perform addition in the interpreter.
        int r1 = x + y;
        int r2 = x + 1;
        int r3 = 2147483647 + z;

        // CHECK: 5
        print(r1);
        // CHECK: 4
        print(r2);
        // CHECK: -2147483648
        print(r3);
    }
}
