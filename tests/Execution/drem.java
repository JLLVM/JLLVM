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
        print(nan % p_123);
        print(n_123 % nan);

        // CHECK: 0.456
        // CHECK: -0.456
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

        var x = 3.52095328E9;
        var y = -5.9460864E8;
        var z = 1425.10118;

        // CHECK: 547910080
        print(x % y);
        // CHECK: 1397.81175972065
        print(x % z);
        // CHECK: -848.757979952823
        print(y % z);
    }
}
