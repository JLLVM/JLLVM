// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 3;
        int y = 2;
        int z = 1;
        int a = -10;

        int[] arr = {x, y, z, a};
        // CHECK: 3
        print(arr[0]);
        // CHECK: 2
        print(arr[1]);
        // CHECK: 1
        print(arr[2]);
        // CHECK: -10
        print(arr[3]);
        // CHECK: 4
        print(arr.length);
    }
}
