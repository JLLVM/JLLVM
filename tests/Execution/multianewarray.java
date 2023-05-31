// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean b);
    public static native void print(int i);
    public static native void print(String s);

    public static void main(String[] args)
    {
        var arr = new int[2][3][4];

         // CHECK: 4
        print(arr[0][0].length);

        var arr1 = new int[2][2][2][2];

        // CHECK-COUNT-16: 0
        for(var arr2 : arr)
        {
            for(var arr3 : arr2)
            {
                for(var i : arr3)
                {
                    print(i);
                }
            }
        }

        var arr2 = new String[10][40];

        arr2[5][20] = "Test";

        // CHECK: 1
        print(arr2[4][20] == null);

        // CHECK: 1
        print(arr2[5][20] == "Test");
    }
}
