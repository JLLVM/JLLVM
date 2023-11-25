// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class
// RUN: jllvm -Xint %t/Test.class

class Test
{
    public static native void print(String s);

    public static void main(String[] args)
    {
        var o = new Object();
        synchronized(o)
        {
            new Object();
        }

        o = null;

        try
        {
            synchronized(o)
            {
                new Object();
            }
        }
        catch(NullPointerException e)
        {
            // CHECK: monitor null
            print("monitor null");
        }
    }
}
