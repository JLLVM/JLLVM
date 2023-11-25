// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public int i = 5;

    public static native void print(int i);
    public static native void print(String s);

    public static void main(String[] args)
    {
        var t = new Test();
        // CHECK: 5
        print(t.i);
        t.i = 3;
        // CHECK: 3
        print(t.i);

        t = null;

        try
        {
            var i = t.i;
        }
        catch(NullPointerException e)
        {
            // CHECK: get null
            print("get null");
        }

        try
        {
            t.i = 1;
        }
        catch(NullPointerException e)
        {
            // CHECK: put null
            print("put null");
        }
    }
}
