// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(long l);

    public static void main(String[] args)
    {
        long x = 3l;
        long y = 2l;
        // CHECK: 1
        // CHECK-NOT:-
        print(x/y);
        // CHECK: 3
        // CHECK-NOT:-
        print(x/1l);
        // CHECK: 9
        // CHECK-NOT:-
        print(27l/x);
        // CHECK: 9
        // CHECK-NOT:-
        print(28l/x);
        // CHECK: 0
        // CHECK-NOT:-
        print(Long.MAX_VALUE/Long.MIN_VALUE);
        // CHECK: -3
        print(x/-1l);
        // CHECK: 0
        // CHECK-NOT:-
        print(-1l/x);
        // CHECK: -5
        print(-15l/3l);
    }
}
