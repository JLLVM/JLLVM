// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t && javac %t/Other.java -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

//--- Test.java

class Test
{
    public static native void print(String s);

    public static void main(String[] args) throws ClassNotFoundException
    {
        // CHECK: Before
        Test.print("Before");

        // CHECK: Other clinit run
        Class.forName("Other", /*initialize=*/true, /*loader=*/null);

        // CHECK-NOT: Third clinit run
        Class.forName("Third", /*initialize=*/false, /*loader=*/null);

        // CHECK: After
        Test.print("After");

        // CHECK: Third clinit run
        Third.nothing();

        Class array = Class.forName("[LOther;");
        // CHECK: [LOther;
        Test.print(array.getName());
    }
}

//--- Other.java

public class Other
{
    static {
        Test.print("Other clinit run");
    }
}

//--- Third.java

public class Third
{
    static {
        Test.print("Third clinit run");
    }

    public static void nothing()
    {}
}
