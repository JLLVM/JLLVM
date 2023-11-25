// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

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
        // CHECK: 5
        o.foo();
    }

    private void foo()
    {
        print(5);
    }
}


//--- Other.java

public class Other extends Test
{
    public void foo()
    {
        Test.print(6);
    }
}
