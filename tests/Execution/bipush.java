// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        byte b = 6;
        // CHECK: 6
        print(b);
        byte b2 = -15;
        // CHECK: -15
        print(b2);
    }
}
