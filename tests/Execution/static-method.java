// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void printValue()
    {
        print(5);
    }

    public static void main(String[] args)
    {
        // CHECK: 5
        printValue();
    }
}
