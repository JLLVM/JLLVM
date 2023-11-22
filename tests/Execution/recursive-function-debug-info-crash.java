// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class
// RUN: jllvm -Xint %t/Test.class

class Test
{
    public static native void print(int i);

    public static int fib(int i)
    {
        if (i == 0 || i == 1)
        {
            return i;
        }
        return fib(i - 1) + fib(i - 2);
    }

    public static void main(String[] args)
    {
        // CHECK: 8
        print(fib(6));
    }
}
