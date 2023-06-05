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

        var x = -4.4639528E7;
        var y = 1.26373286E9;
        var z = -238.710592;

        // CHECK: -0.0353235477314406
        print(x / y);
        // CHECK: 187002.711635016
        print(x / z);
        // CHECK: -5293995.75197736
        print(y / z);
    }
}
