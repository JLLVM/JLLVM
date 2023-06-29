// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 3;
        int y = 2;
        int a = -40;
        int b = 5601;
        // CHECK: 1
        print(x%y);
        // CHECK: 0
        print(x%1);
        // CHECK: 0
        print(27%x);
        // CHECK: 1
        print(28%x);
        // CHECK: -40
        print(a%b);
        // CHECK: 1
        print(b%a);
    }
}
