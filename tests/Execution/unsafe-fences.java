// RUN: javac --add-exports java.base/jdk.internal.misc=ALL-UNNAMED %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

import jdk.internal.misc.Unsafe;

class Test
{
    private static final Unsafe u = Unsafe.getUnsafe();

    static native void print(String s);

    public static void main(String[] args)
    {
        // Check that calls to these work at least. Can't really test their side effect.
        u.loadFence();
        u.storeFence();
        u.fullFence();

        // CHECK: Success
        print("Success");
    }
}
