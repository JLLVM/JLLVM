// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class

class Test
{
    public static int x;

    private void dummy()
    {
        assert x == 5;
    }

    public static void main(String[] args)
    {
        // Nothing for now, just making sure the implicit '<clinit>' works
    }
}
