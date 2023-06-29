// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(float f);

    public static void main(String[] args)
    {
        foo();
    }

    public static void foo()
    {
        var f0 = 0.0f;
        var f1 = 1.0f;
        var f2 = 2.0f;
        var f3 = f123();
        var f4 = fm987();

        // CHECK: 0
        print(f0);
        // CHECK: 1
        print(f1);
        // CHECK: 2
        print(f2);
        // CHECK: 123.456
        print(f3);
        // CHECK: -987.654
        print(f4);
    }

    public static float f123()
    {
        return 123.456f;
    }

    public static float fm987()
    {
        return -987.654f;
    }
}
