// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

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
        print(nan * p_123);
        print(p_123 * nan);

        // CHECK: 15241.383936
        // CHECK: 15241.383936
        print(p_123 * p_123);
        print(n_123 * n_123);

        // CHECK: -15241.383936
        // CHECK: -15241.383936
        print(p_123 * n_123);
        print(n_123 * p_123);

        // CHECK: nan
        // CHECK: nan
        print(p_inf * p_zero);
        print(n_inf * n_zero);

        // CHECK: inf
        // CHECK: -inf
        print(p_inf * p_123);
        print(n_inf * p_123);

        var x = 6.7671592E7;
        var y = -4.09644288E8;
        var z = 1374.04144;

        // CHECK: -2.77212811226665e+16
        print(x * y);
        // CHECK: 92983571718.7725
        print(x * z);
        // CHECK: -562868227371.295
        print(y * z);
    }
}
