// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        var arr1 = new Object[0];
        var arr2 = new Object[0];

        // CHECK: 0
        print(arr1.equals(arr2));
        // CHECK: 1
        print(arr1.equals(arr1));
    }
}
