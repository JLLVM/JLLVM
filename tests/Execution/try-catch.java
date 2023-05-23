// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

class Test
{
    public static void foo()
    {}

    public static void main(String[] args)
    {
        try
        {
            foo();
        }
        catch (Exception e)
        {
            e.printStackTrace();
        }
    }
}
