// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(long i);

    public static void main(String[] args)
    {
        long x = 5l;
        long y = 2l;
        long z = -1l;
        long min = Long.MIN_VALUE;
        long max = Long.MAX_VALUE;

        //CHECK: 20
        print(x << 2);
        //CHECK: 64
        print(y << 5);
        //CHECK: -4
        print(z << 2);

        //CHECK: 1
        print(x >> 2);
        //CHECK: 0
        print(y >> 5);
        //CHECK: -1
        print(z >> 2);

        //CHECK: 1
        print(x >>> 2);
        //CHECK: 0
        print(y >>> 5);
        //CHECK: 4611686018427387903
        print(z >>> 2);

        //CHECK: 2305843009213693952
        print(min >>> 2);
        //CHECK: -2305843009213693952
        print(min >> 2);
        //CHECK: 4
        print(max << 2);
    }
}
