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

        var x = 1.06639384E8f;
        var y = -5.4427053E8f;
        var z = -4.29113472f;

        // CHECK: -1.06639e+08
        print(-x);
        // CHECK: 5.44271e+08
        print(-y);
        // CHECK: 4.29113
        print(-z);
    }
}
