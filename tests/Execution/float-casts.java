// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(double d);
    public static native void print(long l);

    public static void main(String[] args)
    {
        var nan = Float.NaN;
        var p_inf = Float.POSITIVE_INFINITY;
        var n_inf = Float.NEGATIVE_INFINITY;
        var p_zero = 0.0f;
        var n_zero = -0.0f;
        var p_19 = 1.9f;
        var n_19 = -1.9f;
        var p_long_max_plus = 9323372036854775807.0f;
        var n_long_max_plus = -9323372036854775807.0f;

        //CHECK: nan
        print((double) nan);
        //CHECK: inf
        print((double) p_inf);
        //CHECK: -inf
        print((double) n_inf);
        //CHECK: 0
        print((double) p_zero);
        //CHECK: -0
        print((double) n_zero);
        //CHECK: 9.32337151988938e+18
        print((double) p_long_max_plus);
        //CHECK: -9.32337151988938e+18
        print((double) n_long_max_plus);

        //CHECK: 0
        print((int) nan);
        //CHECK: 2147483647
        print((int) p_inf);
        //CHECK: -2147483648
        print((int) n_inf);
        //CHECK: 0
        print((int) p_zero);
        //CHECK: 0
        print((int) n_zero);
        //CHECK: 1
        print((int) p_19);
        //CHECK: -1
        print((int) n_19);
        //CHECK: 2147483647
        print((int) p_long_max_plus);
        //CHECK: -2147483648
        print((int) n_long_max_plus);

        //CHECK: 0
        print((long) nan);
        //CHECK: 9223372036854775807
        print((long) p_inf);
        //CHECK: -9223372036854775808
        print((long) n_inf);
        //CHECK: 0
        print((long) p_zero);
        //CHECK: 0
        print((long) n_zero);
        //CHECK: 1
        print((long) p_19);
        //CHECK: -1
        print((long) n_19);
        //CHECK: 9223372036854775807
        print((long) p_long_max_plus);
        //CHECK: -9223372036854775808
        print((long) n_long_max_plus);
    }
}
