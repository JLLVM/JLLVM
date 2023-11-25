// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(double d);
    public static native void print(float f);

    public static void main(String[] args)
    {
        long x = (long)Integer.MAX_VALUE+1;
        long y = Long.MAX_VALUE;
        long z = Long.MIN_VALUE;

        //CHECK: -2147483648
        print((int) x);
        //CHECK: -1
        print((int) y);
        //CHECK: 0
        print((int) z);

        //CHECK: 2.14748e+09
        print((float) x);
        //CHECK: 9.22337e+18
        print((float) y);
        //CHECK: -9.22337e+18
        print((float) z);

        //CHECK: 2147483648
        print((double) x);
        //CHECK: 9.22337203685478e+18
        print((double) y);
        //CHECK: -9.22337203685478e+18
        print((double) z);
    }
}
