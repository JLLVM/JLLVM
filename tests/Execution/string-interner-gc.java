// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(String i);
    public static native void print(int i);

    public static void usesStringLiteral()
    {
        print("a string literal");
    }

    public static void main(String[] args)
    {
        var i = new int[5];
        // CHECK: a string literal
        usesStringLiteral();
        // CHECK: 5
        print(i.length);
    }
}
