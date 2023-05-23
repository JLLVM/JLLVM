// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(byte b);
    public static native void print(char c);
    public static native void print(short s);
    public static native void print(float f);
    public static native void print(double d);
    public static native void print(long l);

    public static void main(String[] args)
    {
        int x = 516;
        int y = 129;
        int z = -129;

        //CHECK: 4
        print((byte) x);
        //CHECK: -127
        print((byte) y);
        //CHECK: 127
        print((byte) z);

        //CHECK: 516
        print((short) x);
        //CHECK: 129
        print((short) y);
        //CHECK: -129
        print((short) z);

        //CHECK: 516
        print((char) x);
        //CHECK: 129
        print((char) y);
        //CHECK: 65407
        print((char) z);

        //CHECK: 516
        print((long) x);
        //CHECK: 129
        print((long) y);
        //CHECK: -129
        print((long) z);

        //CHECK: 516
        print((float) x);
        //CHECK: 129
        print((float) y);
        //CHECK: -129
        print((float) z);

        //CHECK: 516
        print((double) x);
        //CHECK: 129
        print((double) y);
        //CHECK: -129
        print((double) z);

        // Testing narrowing conversions
        int b = 2 * Byte.MAX_VALUE;
        //CHECK: -2
        print((int)(byte) b);

        int c = 2 * Character.MAX_VALUE;
        //CHECK: 65534
        print((int)(char) c);

        int s = 2 * Short.MAX_VALUE;
        //CHECK: -2
        print((int)(short) s);
    }
}
