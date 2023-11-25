// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(float f);

    public static void main(String[] args)
    {
        var nan = Float.NaN;
        var p_inf = Float.POSITIVE_INFINITY;
        var n_inf = Float.NEGATIVE_INFINITY;
        var p_zero = 0.0f;
        var n_zero = -0.0f;
        var p_123 = 123.456f;
        var n_123 = -123.456f;

        // CHECK: nan
        // CHECK: nan
        print(nan * p_123);
        print(p_123 * nan);

        // CHECK: 15241.4
        // CHECK: 15241.4
        print(p_123 * p_123);
        print(n_123 * n_123);

        // CHECK: -15241.4
        // CHECK: -15241.4
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

        var x = 6.7671592E7f;
        var y = -4.09644288E8f;
        var z = 1374.04144f;

        // CHECK: -2.77213e+16
        print(x * y);
        // CHECK: 9.29836e+10
        print(x * z);
        // CHECK: -5.62868e+11
        print(y * z);
    }
}
