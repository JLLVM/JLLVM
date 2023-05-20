// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

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

        // CHECK: nan
        // CHECK: nan
        print(nan / 2);
        print(2 / nan);

        // CHECK: nan
        // CHECK: nan
        print(p_inf / n_inf);
        print(n_inf / p_inf);

        // CHECK: inf
        // CHECK: -inf
        print(p_inf / 2);
        print(n_inf / 2);

        // CHECK: 0
        // CHECK: -0
        print(2 / p_inf);
        print(2 / n_inf);

        // CHECK: nan
        // CHECK: nan
        print(p_zero / n_zero);
        print(n_zero / p_zero);

        // CHECK: 0
        // CHECK: -0
        print(p_zero / 2);
        print(n_zero / 2);

        // CHECK: inf
        // CHECK: -inf
        print(2 / p_zero);
        print(2 / n_zero);

        var x = -4.4639528E7f;
        var y = 1.26373286E9f;
        var z = -238.710592f;

        // CHECK: -0.0353235
        print(x / y);
        // CHECK: 187003
        print(x / z);
        // CHECK: -5.294e+06
        print(y / z);
    }
}
