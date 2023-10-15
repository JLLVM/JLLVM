// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(String s);

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

        arr = null;

        try
        {
            var i = arr[0];
        }
        catch(NullPointerException e)
        {
            // CHECK: load null
            print("load null");
        }

        try
        {
            arr[0] = 1;
        }
        catch(NullPointerException e)
        {
            // CHECK: store null
            print("store null");
        }

        try
        {
            var l = arr.length;
        }
        catch(NullPointerException e)
        {
            // CHECK: length null
            print("length null");
        }

        arr = new int[3];

        try
        {
            var i = arr[-30];
        }
        catch(ArrayIndexOutOfBoundsException e)
        {
            // CHECK: load under
            print("load under");
        }

        try
        {
            arr[20] = 9;
        }
        catch(ArrayIndexOutOfBoundsException e)
        {
            // CHECK: store over
            print("store over");
        }
    }
}
