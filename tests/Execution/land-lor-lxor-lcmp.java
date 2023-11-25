// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(long l);
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        long x = 1l;
        long max = Long.MAX_VALUE;
        long min = Long.MIN_VALUE;
        long y = 4l;
        long z = 5l;

        //CHECK: 1
        print(x & max);
        //CHECK: 0
        print(x & min);
        //CHECK: 0
        print(min & max);
        //CHECK: 4
        print(y & z);

        //CHECK: 9223372036854775807
        print(x | max);
        //CHECK: -9223372036854775807
        print(x | min);
        //CHECK: -1
        print(min | max);
        //CHECK: 5
        print(y | z);

        //CHECK: 9223372036854775806
        print(x ^ max);
        //CHECK: -9223372036854775807
        print(x ^ min);
        //CHECK: -1
        print(min ^ max);
        //CHECK: 1
        print(y ^ z);

        // CHECK: 1
        print(max == Long.MAX_VALUE);
        //CHECK: 1
        print(min < max);
        //CHECK: 0
        print(y > z);
    }
}
