// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

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
        print(-nan);

        // CHECK: -inf
        // CHECK: inf
        print(-p_inf);
        print(-n_inf);

        // CHECK: -0
        // CHECK: 0
        print(-p_zero);
        print(-n_zero);

        // CHECK: -123.456
        // CHECK: 123.456
        print(-p_123);
        print(-n_123);

        var x = 1.06639384E8;
        var y = -5.4427053E8;
        var z = -4.29113472;

        // CHECK: -106639384
        print(-x);
        // CHECK: 544270530
        print(-y);
        // CHECK: 4.29113472
        print(-z);
    }
}
