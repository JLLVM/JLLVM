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
        long z = 1l;
        // CHECK: 6
        print(x*y);
        // CHECK: 3
        print(x*1l);
        // CHECK: -9223372036854775808
        print(Long.MAX_VALUE*Long.MIN_VALUE);
    }
}
