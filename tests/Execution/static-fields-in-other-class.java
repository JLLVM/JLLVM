// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 5
        print(Other.i);
        Other.i = 3;
        // CHECK: 3
        print(Other.i);
    }
}

//--- Other.java

public class Other
{
    public static int i = 5;
}
