// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static final boolean z = getZ();
    public static final byte b = getB();
    public static final char c = getC();
    public static final double d = getD();
    public static final float f = getF();
    public static final int i = getI();
    public static final long l = getL();
    public static final short s = getS();

    public static native void print(boolean b);
    public static native void print(byte b);
    public static native void print(char c);
    public static native void print(double f);
    public static native void print(float f);
    public static native void print(int i);
    public static native void print(long l);
    public static native void print(short s);

    public static void main(String[] args)
    {
        // CHECK: 1
        print(Test.z);
        // CHECK: -42
        print(Test.b);
        // CHECK: 67
        print(Test.c);
        // CHECK: 2.71828182846
        print(Test.d);
        // CHECK: 3.14159
        print(Test.f);
        // CHECK: -123456789
        print(Test.i);
        // CHECK: 987654321
        print(Test.l);
        // CHECK: 1234
        print(Test.s);
    }

    public static boolean getZ(){
        return true;
    }

    public static byte getB(){
        return -42;
    }

    public static char getC(){
        return 'C';
    }

    public static double getD(){
        return 2.71828182846d;
    }

    public static float getF(){
        return 3.141592f;
    }

    public static int getI(){
        return -123456789;
    }

    public static long getL(){
        return 987654321;
    }

    public static short getS(){
        return 1234;
    }
}
