// RUN: javac --add-exports java.base/jdk.internal.misc=ALL-UNNAMED %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

import jdk.internal.misc.Unsafe;

class Test
{
    private static final Unsafe u = Unsafe.getUnsafe();

    private static long Z_OFFSET;
    private static long B_OFFSET;
    private static long S_OFFSET;
    private static long C_OFFSET;
    private static long I_OFFSET;
    private static long L_OFFSET;
    private static long T_OFFSET;

    static {
        Z_OFFSET = u.objectFieldOffset(Test.class, "z");
        B_OFFSET = u.objectFieldOffset(Test.class, "b");
        S_OFFSET = u.objectFieldOffset(Test.class, "s");
        C_OFFSET = u.objectFieldOffset(Test.class, "c");
        I_OFFSET = u.objectFieldOffset(Test.class, "i");
        L_OFFSET = u.objectFieldOffset(Test.class, "l");
        T_OFFSET = u.objectFieldOffset(Test.class, "t");
    }

    public boolean z = true;
    public byte b = 7;
    public short s = 7;
    public char c = 'x';
    public int i = 7;
    public long l = 7;
    public Test t = null;

    static native void print(String s);

    public static void main(String[] args)
    {
        var t = new Test();

        if (u.getReferenceVolatile(t, T_OFFSET) == null)
        {
            // CHECK: Success
            print("Success");
        }
        var tmp = new Test();
        u.putReferenceVolatile(t, T_OFFSET, tmp);
        if (t.t == tmp)
        {
            // CHECK: Success
            print("Success");
        }

        if (u.getIntVolatile(t, I_OFFSET) == 7)
        {
            // CHECK: Success
            print("Success");
        }
        u.putIntVolatile(t, I_OFFSET, 96);
        if (t.i == 96)
        {
            // CHECK: Success
            print("Success");
        }
    }
}
