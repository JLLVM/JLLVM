// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(long l);

    public static void main(String[] args)
    {
        long x = 3l;
        long y = 2l;
        long a = -40l;
        long b = 5601l;
        // CHECK: 1
        print(x%y);
        // CHECK: 0
        print(x%1l);
        // CHECK: 0
        print(27l%x);
        // CHECK: 1
        print(28l%x);
        // CHECK: -40
        print(a%b);
        // CHECK: 1
        print(b%a);
    }
}
