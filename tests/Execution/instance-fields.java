// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public int i = 5;

    public static native void print(int i);

    public static void main(String[] args)
    {
        var t = new Test();
        // CHECK: 5
        print(t.i);
        t.i = 3;
        // CHECK: 3
        print(t.i);
    }
}
