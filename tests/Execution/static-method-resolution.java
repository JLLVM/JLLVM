// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test extends Other implements Foo
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 5
        printValue();
        // CHECK: 9
        new Test().printValueI();
    }
}

class Other
{
    public static void printValue()
    {
        Test.print(5);
    }
}

interface Foo
{
    default void printValueI()
    {
        printValueFoo();
    }

    public static void printValueFoo()
    {
        Test.print(9);
    }
}
