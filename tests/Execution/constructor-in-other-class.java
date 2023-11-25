// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xjit %t/Other.class | FileCheck %s
// RUN: jllvm -Xint %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public Test()
    {
        print(420000000);
    }
}

// CHECK: 420000000

//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        new Test();
    }
}
