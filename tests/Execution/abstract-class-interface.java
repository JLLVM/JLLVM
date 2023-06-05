// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm -Xenable-test-utils %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);
}

//--- A.java

public interface A
{
    void a();

    void b();
}

//--- B.java

public abstract class B implements A
{
    public abstract void a();

    // b not declared here!
}

//--- C.java

public class C extends B
{
    public void a()
    {
        Test.print(6);
    }

    public void b()
    {
        Test.print(9);
    }
}

//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        B c = new C();
        // CHECK: 6
        c.a();
        // CHECK: 9
        c.b();
    }
}
