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
        // CHECK: 5
        print(x+y);
        // CHECK: 4
        print(x+1l);
        // CHECK: -9223372036854775808
        print(Long.MAX_VALUE+z);
    }
}
