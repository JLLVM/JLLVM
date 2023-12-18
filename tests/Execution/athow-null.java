// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(String s);

    public static void main(String[] args)
    {
        try
        {
            testThrow(null);
        }
        catch(IllegalArgumentException e)
        {
            // CHECK-NOT: wrong
            print("wrong");
        }
        catch(NullPointerException e)
        {
            // CHECK: caught null
            print("caught null");
        }
    }

    private static void testThrow(IllegalArgumentException e)
    {
        throw e;
    }
}
