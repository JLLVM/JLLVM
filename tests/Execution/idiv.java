// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 3;
        int y = 2;
        // CHECK: 1
        print(x/y);
        // CHECK: 3
        print(x/1);
        // CHECK: 9
        print(27/x);
        // CHECK: 9
        print(28/x);
        // CHECK: 0
        print(Integer.MAX_VALUE/Integer.MIN_VALUE);
    }
}
