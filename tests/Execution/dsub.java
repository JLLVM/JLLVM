// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(double d);

    public static void main(String[] args)
    {
        var nan = Double.NaN;
        var p_inf = Double.POSITIVE_INFINITY;
        var n_inf = Double.NEGATIVE_INFINITY;
        var p_zero = 0.0;
        var n_zero = -0.0;
        var p_123 = 123.456;
        var n_123 = -123.456;

        // CHECK: nan
        // CHECK: nan
        print(nan - 1);
        print(1 - nan);

        // CHECK: inf
        // CHECK: -inf
        print(p_inf - n_inf);
        print(n_inf - p_inf);

        // CHECK: nan
        // CHECK: nan
        print(p_inf - p_inf);
        print(n_inf - n_inf);

        // CHECK: inf
        // CHECK: -inf
        print(p_inf - 1);
        print(n_inf - 1);

        // CHECK: -inf
        // CHECK: inf
        print(1 - p_inf);
        print(1 - n_inf);

        // CHECK: 0
        // CHECK: -0
        print(p_zero - n_zero);
        print(n_zero - p_zero);

        // CHECK: 0
        // CHECK: 0
        print(p_zero - p_zero);
        print(n_zero - n_zero);

        // CHECK: -123.456
        // CHECK: -123.456
        print(p_zero - p_123);
        print(n_zero - p_123);

        // CHECK: 246.912
        // CHECK: -246.912
        print(p_123 - n_123);
        print(n_123 - p_123);

        var x = 1.83799936E8;
        var y = -1.09827904E9;
        var z = 1.1562734E7;

        // CHECK: 1.28208e+09
        print(x - y);
        // CHECK: 1.72237e+08
        print(x - z);
        // CHECK: -1.10984e+09
        print(y - z);
    }
}
