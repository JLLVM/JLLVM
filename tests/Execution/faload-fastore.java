// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(float f);

    public static void main(String[] args)
    {
        var nan = Float.NaN;
        var p_inf = Float.POSITIVE_INFINITY;
        var n_zero = -0.0f;
        var p_123 = 123.456f;

        float[] arr = {nan, p_inf, n_zero, p_123};
        // CHECK: nan
        print(arr[0]);
        // CHECK: inf
        print(arr[1]);
        // CHECK: -0
        print(arr[2]);
        // CHECK: 123.456
        print(arr[3]);
        // CHECK: 4
        print(arr.length);
    }
}
