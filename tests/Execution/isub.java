// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 3;
        int y = 2;
        int z = 1;
        // CHECK-NOT:-
        // CHECK: 1
        print(x-y);
        // CHECK-NOT:-
        // CHECK: 2
        print(x-1);
        // CHECK-NOT:-
        // CHECK: 2147483647
        print(Integer.MIN_VALUE-z);
        // CHECK: -1
        print(y-x);
    }
}
