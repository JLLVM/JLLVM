// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

//--- Test.java

public class Test implements Other
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        var t = new Test();
        // CHECK: 5
        t.a();
    }
}

//--- Other.java

public interface Other
{
    default void a()
    {
        Test.print(5);
    }
}
