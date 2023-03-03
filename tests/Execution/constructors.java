// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public Test()
    {
        print(420000000);
    }

    public static void main(String[] args)
    {
        new Test();
    }
}

// CHECK: 420000000
