// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t && javac %t/Other.java -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

//--- Test.java

class Test
{
    public static native void print(String s);

    public static void main(String[] args) throws ClassNotFoundException
    {
        // CHECK: Before
        Test.print("Before");

        // CHECK: Other clinit run
        new Other();

        // CHECK: After
        Test.print("After");

        // CHECK-NOT: Other clinit run
        Other.nothing();
    }
}

//--- Other.java

public class Other
{
    static {
        Test.print("Other clinit run");
    }

    public static void nothing()
    {
    }
}
