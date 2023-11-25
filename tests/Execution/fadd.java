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
        print(nan + 1);
        print(1 + nan);

        // CHECK: nan
        // CHECK: nan
        print(p_inf + n_inf);
        print(n_inf + p_inf);

        // CHECK: inf
        // CHECK: -inf
        print(p_inf + p_inf);
        print(n_inf + n_inf);

        // CHECK: inf
        // CHECK: -inf
        print(p_inf + 1);
        print(n_inf + 1);

        // CHECK: 0
        // CHECK: 0
        print(p_zero + n_zero);
        print(n_zero + p_zero);

        // CHECK: 0
        // CHECK: -0
        print(p_zero + p_zero);
        print(n_zero + n_zero);

        // CHECK: 123.456
        // CHECK: 123.456
        print(p_zero + p_123);
        print(n_zero + p_123);

        // CHECK: 0
        // CHECK: 0
        print(p_123 + n_123);
        print(n_123 + p_123);

        var x = -6.6057786f;
        var y = 1549700.4f;
        var z = -2.1339336E8f;

        // CHECK: 1.54969e+06
        print(x + y);
        // CHECK: -2.13393e+08
        print(x + z);
        // CHECK: -2.11844e+08
        print(y + z);
    }
}
