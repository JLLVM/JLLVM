// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xenable-test-utils %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void foo(int[] i)
    {
        i[0] = 3;
    }

    // Creates a <clinit> that does GC.
    private static Object s = new Object();
}


//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        var i = new int[1];
        // First time call to 'Test', executes <clinit>.
        Test.foo(i);
        // Stub that called <clinit> before 'foo' should have properly relocated 'i' and changes from 'foo' should be
        // visible in this 'i'.
        // CHECK: 3
        Test.print(i[0]);
    }
}
