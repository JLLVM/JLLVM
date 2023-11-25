// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xjit %t/Other.class | FileCheck %s
// RUN: jllvm -Xint %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void printValue()
    {
        print(5);
    }
}


//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        // CHECK: 5
        Test.printValue();
    }
}
