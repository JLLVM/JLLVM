// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);
}

//--- A.java

public class A
{
    public final void a()
    {
        Test.print(5);
    }
}

//--- Other.java

public class Other extends A
{
    public static void main(String[] args)
    {
        Other c = new Other();
        // CHECK: 5
        c.a();
    }
}
