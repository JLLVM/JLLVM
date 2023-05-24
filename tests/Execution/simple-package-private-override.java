// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        Other t = new Other();
        // CHECK: 6
        t.foo();
        Test o = t;
        // CHECK: 6
        o.foo();
    }

    void foo()
    {
        print(5);
    }
}


//--- Other.java

public class Other extends Test
{
    void foo()
    {
        Test.print(6);
    }
}
