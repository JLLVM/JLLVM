// RUN: javac %s -d %t
// RUN: not jllvm %t/Test.class 2>&1 | FileCheck %s

class Test
{
    public static native void print(String s);

    public static native Object test(boolean b, String s, int[] arr);

    // CHECK: java.lang.UnsatisfiedLinkError: java.lang.Object Test.test(boolean, java.lang.String, int[])

    public static void main(String[] args)
    {
        try
        {
            test(true, "String", null);
        }
        catch (UnsatisfiedLinkError e)
        {
            // CHECK: Caught for test
            print("Caught for test");
        }

        test(true, "String", null);
    }
}
