// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(byte b);
    public static native void print(char c);
    public static native void print(double d);
    public static native void print(float f);
    public static native void print(int i);
    public static native void print(long l);
    public static native void print(short s);
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        byte b = -1;
        char c = 0; // Java char does not support assignment from negative values
        double d = 1.0;
        float f = 2.0f;
        int i = 3;
        long l = 4;
        short s = 5;
        boolean z = false;

        // CHECK: -1
        print(b);
        // CHECK: 0
        print(c);
        // CHECK: 1
        print(d);
        // CHECK: 2
        print(f);
        // CHECK: 3
        print(i);
        // CHECK: 4
        print(l);
        // CHECK: 5
        print(s);
        // CHECK: 0
        print(z);
    }
}
