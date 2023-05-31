// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

//--- Test.java

class Test
{
    public static native void print(String i);
    public static native void print(int i);

    public static void main(String[] args)
    {
        var i = new int[5];
        // All these operations are the only operations that can trigger class object initialization.
        Other.foo();
        Other2.foo = Other3.foo;
        new Other4();
        // CHECK: 5
        print(i.length);
    }
}

//--- Other.java

public class Other
{
    static
    {
        // Trigger GC
        new Object();
        new Object();
        new Object();
        new Object();
        new Object();
    }

    public static void foo()
    {

    }
}

//--- Other2.java

public class Other2
{
    static
    {
        // Trigger GC
        new Object();
        new Object();
        new Object();
        new Object();
        new Object();
    }

    public static int foo = 3;
}

//--- Other3.java

public class Other3
{
    static
    {
        // Trigger GC
        new Object();
        new Object();
        new Object();
        new Object();
        new Object();
    }

    public static int foo = 3;
}

//--- Other4.java

public class Other4
{
    static
    {
        // Trigger GC
        new Object();
        new Object();
        new Object();
        new Object();
        new Object();
    }
}
