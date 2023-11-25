// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(float f);
    public static native void print(long l);

    public static void main(String[] args)
    {
        var nan = Double.NaN;
        var p_inf = Double.POSITIVE_INFINITY;
        var n_inf = Double.NEGATIVE_INFINITY;
        var p_zero = 0.0;
        var n_zero = -0.0;
        var p_19 = 1.9;
        var n_19 = -1.9;
        var p_long_max_plus = 9323372036854775807.0;
        var n_long_max_plus = -9323372036854775807.0;
        var p_float_max_plus = 3.4028235E40;
        var n_float_max_plus = -3.4028235E40;

        //CHECK: nan
        print((float) nan);
        //CHECK: inf
        print((float) p_inf);
        //CHECK: -inf
        print((float) n_inf);
        //CHECK: 0
        print((float) p_zero);
        //CHECK: -0
        print((float) n_zero);
        //CHECK: inf
        print((float) p_float_max_plus);
        //CHECK: -inf
        print((float) n_float_max_plus);

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
