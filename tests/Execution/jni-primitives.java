// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(byte i);
    public static native void print(char i);
    public static native void print(double i);
    public static native void print(float i);
    public static native void print(int i);
    public static native void print(long i);
    public static native void print(short i);
    public static native void print(boolean i);

    public static void main(String[] args)
    {
        byte b = -1;
        print(b);
        short s = -1;
        print(s);
        int i = -1;
        print(i);

        // TODO: Needs 'ldc2_w' https://github.com/JLLVM/JLLVM/issues/17
        // long l = -1;
        // print(l);

        // Java char does not support assignment from negative values
        char c = 5;
        print(5);

        print(true);

        print(0.0f);
    }
}

// CHECK-COUNT-3: -1
// CHECK: 5
// CHECK: 1
// CHECK: 0
