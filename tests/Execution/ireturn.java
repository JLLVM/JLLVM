// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(boolean i);
    public static native void print(short i);
    public static native void print(char i);

    private static int test1()
    {
        return 5;
    }

    private static int test2()
    {
        return -3;
    }

    private static boolean test3()
    {
        return true;
    }

    private static short test4()
    {
        return 5;
    }

    private static char test5()
    {
        return 'A';
    }

    public static void main(String[] args)
    {
        // CHECK: 5
        print(test1());
        // CHECK: -3
        print(test2());
        // CHECK: 1
        print(test3());
        // CHECK: 5
        print(test4());
        // CHECK: 65
        print(test5());
    }
}
