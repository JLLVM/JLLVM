// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xjit %t/Other.class | FileCheck %s
// RUN: jllvm -Xint %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);
    public static native void print(String s);
}

//--- A.java

public interface A
{
    default void a()
    {
        Test.print(5);
    }
}

//--- B.java

public interface B extends A
{
    default void b()
    {
        Test.print(6);
    }

    void c();
}

//--- C.java

public class C implements B
{
    public void b()
    {
        Test.print(4);
    }

    public void c()
    {
       B.super.b();
    }
}

//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        B c = new C();
        // CHECK: 5
        c.a();
        // CHECK: 4
        c.b();
        // CHECK: 6
        c.c();

        c = null;

        try
        {
            c.a();
        }
        catch(NullPointerException e)
        {
            // CHECK: interface null
            Test.print("interface null");
        }
    }
}
