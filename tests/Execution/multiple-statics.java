// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t

// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 5
        // CHECK: 3
        print(Other.i);
        print(Otherwise.i);
    }
}

//--- Other.java

public class Other
{
    public static int i = 5;
}

//--- Otherwise.java

public class Otherwise
{
    public static int i = 3;
}
