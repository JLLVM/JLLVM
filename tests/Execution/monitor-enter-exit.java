// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

class Test
{
    public static void main(String[] args)
    {
        var o = new Object();
        synchronized(o)
        {
            new Object();
        }
    }
}
