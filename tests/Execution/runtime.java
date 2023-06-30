// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

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
        var n = Runtime.getRuntime().availableProcessors();
        // CHECK: 1
        print(n >= 1);

        var memory = Runtime.getRuntime().maxMemory();
        // CHECK: 1
        print(memory >= 1);
    }
}
