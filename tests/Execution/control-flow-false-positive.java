// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

class Test
{
    public static int foo()
    {
        // 0xa7FF which looks like a goto to target ...FF.
        return -22529;
    }

    public static void main(String[] args)
    {
        foo();
    }
}
