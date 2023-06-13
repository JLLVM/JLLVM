// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    static boolean B = true;
    static byte BY = 1;
    static char C = 1;
    static double D = 1;
    static float F = 1;
    static int I = 1;
    static long L = 1;
    static short S = 1;

    boolean b = false;
    byte by = 0;
    char c = 0;
    double d = 0;
    float f = 0;
    int i = 0;
    long l = 0;
    short s = 0;

    public static native void print(boolean b);
    public static native void print(byte b);
    public static native void print(char c);
    public static native void print(double d);
    public static native void print(float f);
    public static native void print(int i);
    public static native void print(long l);
    public static native void print(short s);

    public static void main(String[] args)
    {
        // CHECK-COUNT-7: 0
        new Test().print();
        // CHECK-COUNT-7: 1
        print(B);
        print(BY);
        print(C);
        print(D);
        print(I);
        print(L);
        print(S);
    }

    public void print()
    {
        print(b);
        print(by);
        print(c);
        print(d);
        print(i);
        print(l);
        print(s);
    }
}
