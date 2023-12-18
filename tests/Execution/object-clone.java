// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t

// RUN: jllvm -Xint %t/Test.class | FileCheck %s
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s

//--- Test.java

class Test
{
    public static native void print(boolean b);
    public static native void print(String s);
    public static native void print(int i);

    public static void main(String[] args) throws Exception {
        int arr1[] = {1, 2};
        int arr2[] = arr1.clone();
        // CHECK: 0
        print(arr1 == arr2);
        arr1[1]++;
        // CHECK: 2
        print(arr2[1]);

        int arr3[][] = {{1,2}, null};
        int arr4[][] = arr3.clone();
        // CHECK: 1
        print(arr3[0] == arr4[0]);
        // CHECK: 1
        print(arr3[1] == arr4[1]);

        try
        {
            new Test().clone();
        }
        catch(CloneNotSupportedException e)
        {
            // CHECK: Test not Cloneable
            print("Test not Cloneable");
        }

        var o1 = new Other();

        // CHECK-NOT: java.lang.CloneNotSupportedException:
        var o2 = (Other) o1.clone();

        // CHECK: 0
        print(o1 == o2);
        o1.i++;
        // CHECK: 5
        print(o2.i);
    }

    public Object clone() throws CloneNotSupportedException {
        return super.clone();
    }
}

//--- Other.java

public class Other extends Test implements Cloneable
{
    public int i = 5;
}
