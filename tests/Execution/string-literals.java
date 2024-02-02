// RUN: javac -encoding utf-8 %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

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

        // CHECK: 1
        var hello2 = "Hello World";
        print(hello1 == hello2);

        var str1 = new StringBuilder("start").append("end").toString();
        var str2 = new StringBuilder("start").append("end").toString();

        // CHECK: 0
        print(str1 == str2);
        // CHECK: 1
        print(str1.equals(str2));
        // CHECK: 1
        print(hello1.intern() == hello2.intern());
    }
}
