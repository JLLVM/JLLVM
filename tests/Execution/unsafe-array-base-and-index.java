// RUN: javac --add-exports java.base/jdk.internal.misc=ALL-UNNAMED %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

import jdk.internal.misc.Unsafe;

class Test
{
    private static final Unsafe u = Unsafe.getUnsafe();

    static native void print(String s);

    public static void main(String[] args)
    {
        var i = new int[5];
        u.putIntVolatile(i, u.ARRAY_INT_BASE_OFFSET + 3 * u.ARRAY_INT_INDEX_SCALE, 96);
        if (i[3] == 96)
        {
            // CHECK: Success
            print("Success");
        }

        var t = new Test[5];
        var tmp = new Test();
        u.putReferenceVolatile(t, u.ARRAY_OBJECT_BASE_OFFSET + 3 * u.ARRAY_OBJECT_INDEX_SCALE, tmp);
        if (t[3] == tmp)
        {
            // CHECK: Success
            print("Success");
        }
    }
}
