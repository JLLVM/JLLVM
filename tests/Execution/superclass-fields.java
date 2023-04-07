// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Other.class | FileCheck %s

class Test
{
    public int i = 5;
    public static int si = 7;

    public static native void print(int i);
}

class Other extends Test
{
    public static void main(String[] args)
    {
        var t = new Other();
        // CHECK: 5
        Test.print(t.i);
        t.i = 3;
        // CHECK: 3
        Test.print(t.i);

        // CHECK: 7
        Test.print(Other.si);
    }
}
