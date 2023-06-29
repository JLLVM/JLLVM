// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Other.java -d %t
// RUN: jllvm %t/Other.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);
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
    default void a()
    {
        Test.print(6);
    }
}

//--- C.java

public class C implements A, B
{

}

//--- Other.java

public class Other
{
    public static void main(String[] args)
    {
        B c = new C();
        // CHECK: 6
        c.a();
    }
}
