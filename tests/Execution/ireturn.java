// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    private static int test1()
    {
        return 5;
    }

    private static int test2()
        {
            return -3;
        }

    public static void main(String[] args)
    {
        // CHECK: 5
        print(test1());
        // CHECK: -3
        print(test2());
    }
}
