// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static void main(String[] args)
    {
        // CHECK: Hello World!
        System.out.println("Hello World!");
    }
}
