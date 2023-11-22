// RUN: javac %s -d %t
// RUN: not jllvm -Xjit %t/Test.class 2>&1 | FileCheck %s
// RUN: not jllvm -Xint %t/Test.class 2>&1 | FileCheck %s

class Test
{
    public static native void print(String i);

    // CHECK: java.lang.NegativeArraySizeException: -4

    public static void main(String[] args)
    {
        try
        {
            var arr = new int[-1];
        }
        catch (NegativeArraySizeException e)
        {
            // CHECK: Caught for newarray
            print("Caught for newarray");
        }

        try
        {
            var arr = new Test[-2];
        }
        catch (NegativeArraySizeException e)
        {
            // CHECK: Caught for anewarray
            print("Caught for anewarray");
        }

        try
        {
            var arr = new Test[3][-3][4];
        }
        catch (NegativeArraySizeException e)
        {
            // CHECK: Caught for multianewarray
            print("Caught for multianewarray");
        }

        var arr = new Test[-4][-5][4];
    }
}
