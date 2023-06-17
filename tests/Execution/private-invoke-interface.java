// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xenable-test-utils %t/Main.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);
}

//--- Main.java

public interface Main
{
    public static void main(String[] args)
    {
        Other t = new Other();
        // CHECK: 6
        t.foo();
        Main o = t;
        // CHECK: 5
        o.foo();
    }

    private void foo()
    {
        Test.print(5);
    }
}


//--- Other.java

public class Other implements Main
{
    public void foo()
    {
        Test.print(6);
    }
}
