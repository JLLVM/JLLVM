// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

//--- Test.java

public class Test extends Other
{
    public static final int i = Other.i;

    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 2
        print(Other.i);
    }
}

//--- Other.java

public class Other
{
    public static final int i = Test.i + 2;
}
