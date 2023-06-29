// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(long l);

    public static void main(String[] args)
    {
        long x = 3l;
        long y = 2l;
        long z = 1l;
        // CHECK-NOT:-
        // CHECK: 1
        print(x-y);
        // CHECK-NOT:-
        // CHECK: 2
        print(x-1l);
        // CHECK-NOT:-
        // CHECK: 9223372036854775807
        print(Long.MIN_VALUE-z);
        // CHECK: -1
        print(y-x);
    }
}
