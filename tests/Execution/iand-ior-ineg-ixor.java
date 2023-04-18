// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 0;
        int y = Integer.MIN_VALUE;
        int z = 5;
        int a = 3;
        int b = 1;
        // CHECK: 0
        print(x & y);
        // CHECK: 0
        print(b & y);
        // CHECK: -2147483648
        print(y & y);
        // CHECK: 1
        print(z & a);

        // CHECK: -2147483648
        print(x | y);
        // CHECK: -2147483647
        print(b | y);
        // CHECK: -2147483648
        print(y | y);
        // CHECK: 7
        print(z | a);

        // CHECK: -2147483648
        print(x ^ y);
        // CHECK: -2147483647
        print(b ^ y);
        // CHECK: 0
        print(y ^ y);
        // CHECK: 6
        print(z ^ a);

        // CHECK: 0
        print(-x);
        // CHECK: -2147483648
        print(-y);
        // CHECK: -5
        print(-z);
        // CHECK: -3
        print(-a);
        // CHECK: -1
        print(-b);
    }
}
