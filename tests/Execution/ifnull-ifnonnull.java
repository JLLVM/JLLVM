// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        var o = new Object();
        if (o == null)
        {
            // CHECK-NOT: 6
            print(6);
        }
        if (o != null)
        {
            // CHECK: 9
            print(9);
        }
    }
}
