// RUN: javac -encoding utf-8 %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s
class Test
{
    public static native void print(String s);
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        var hello1 = "Hello World";
        // CHECK: Hello World
        print(hello1);
        // CHECK: Hello Java
        print("Hello Java");

        // CHECK: Java == ♨
        print("Java == ♨");

        // TODO: does not currently work, because if_acmpne is either not implemented or not implemented correctly
        /* var hello2 = "Hello World";
        print(hello1 == hello2); */
    }
}
