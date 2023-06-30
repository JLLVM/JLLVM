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
    public void a()
    {
        Test.print(5);
    }
}

//--- B.java

public class B extends A
{
    public void b()
    {
        Test.print(6);
    }
}

//--- C.java

public class C extends B
{
    public void c()
    {
        Test.print(7);
    }

    public void b()
    {
        Test.print(4);
    }
}

//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        C c = new C();
        // CHECK: 5
        c.a();
        // CHECK: 4
        c.b();
        // CHECK: 7
        c.c();
        // CHECK: 4
        ((B)c).b(); // Even when casting, the method of the dynamic type shall be used
    }
}
