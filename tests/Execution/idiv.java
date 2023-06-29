// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 3;
        int y = 2;
        // CHECK: 1
        // CHECK-NOT:-
        print(x/y);
        // CHECK: 3
        // CHECK-NOT:-
        print(x/1);
        // CHECK: 9
        // CHECK-NOT:-
        print(27/x);
        // CHECK: 9
        // CHECK-NOT:-
        print(28/x);
        // CHECK: 0
        // CHECK-NOT:-
        print(Integer.MAX_VALUE/Integer.MIN_VALUE);
        // CHECK: -3
        print(x/-1);
        // CHECK: 0
        // CHECK-NOT:-
        print(-1/x);
        // CHECK: -5
        print(-15/3);
    }
}
