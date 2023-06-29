// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

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
        print(nan % p_123);
        print(n_123 % nan);

        // CHECK: 0.456001
        // CHECK: -0.456001
        print(p_123 % 3);
        print(n_123 % 3);

        // CHECK: nan
        // CHECK: nan
        print(p_inf % 3);
        print(n_inf % 3);

        // CHECK: nan
        // CHECK: nan
        print(p_123 % p_zero);
        print(p_123 % n_zero);

        // CHECK: 123.456
        // CHECK: 123.456
        print(p_123 % p_inf);
        print(p_123 % n_inf);

        // CHECK: 0
        // CHECK: -0
        print(p_zero % p_123);
        print(n_zero % p_123);

        var x = 3.52095328E9f;
        var y = -5.9460864E8f;
        var z = 1425.10118f;

        // CHECK: 5.4791e+08
        print(x % y);
        // CHECK: 1421.57
        print(x % z);
        // CHECK: -841.962
        print(y % z);
    }
}
