// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean b);
    public static native void print(double b);
    public static native void print(float b);

    public static void main(String[] args)
    {
        // CHECK: 1
        print(Double.doubleToRawLongBits(Double.POSITIVE_INFINITY) == 0x7ff0000000000000L);

        // CHECK: 1
        print(Double.doubleToRawLongBits(Double.NEGATIVE_INFINITY) == 0xfff0000000000000L);

        // CHECK: 1
        print(Double.doubleToRawLongBits(Double.NaN) == 0x7ff8000000000000L);

        // CHECK: 1
        print(Double.doubleToRawLongBits(-5.25) == 0xC015000000000000L);

        // CHECK: -5.25
        print(Double.longBitsToDouble(Double.doubleToRawLongBits(-5.25)));

        // CHECK: 1
        print(Double.longBitsToDouble(0xfff0000000000000L) == Double.NEGATIVE_INFINITY);



        // CHECK: 1
        print(Float.floatToRawIntBits(Float.POSITIVE_INFINITY) == 0x7f800000);

        // CHECK: 1
        print(Float.floatToRawIntBits(Float.NEGATIVE_INFINITY) == 0xff800000);

        // CHECK: 1
        print(Float.floatToRawIntBits(Float.NaN) == 0x7fc00000);

        // CHECK: 1
        print(Float.floatToRawIntBits(-5.25f) == 0xc0a80000);

        // CHECK: -5.25
        print(Float.intBitsToFloat(Float.floatToRawIntBits(-5.25f)));

        // CHECK: 1
        print(Float.intBitsToFloat(0xff800000) == Float.NEGATIVE_INFINITY);
    }
}
