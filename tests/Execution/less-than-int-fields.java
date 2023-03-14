// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(byte i);
    public static native void print(short i);
    public static native void print(char i);
    public static native void print(int i);

    public static byte sb = 5;
    public byte b = 5;
    public static short ss = 5;
    public short s = 5;
    public static char sc = 5;
    public char c = 5;

    public static void main(String[] args)
    {
        var t = new Test();
        print((int)Test.sb);
        print((int)t.b);
        print((int)Test.ss);
        print((int)t.s);
        print((int)Test.sc);
        print((int)t.c);
        // CHECK-COUNT-6: 5
    }
}
