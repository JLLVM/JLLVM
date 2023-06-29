// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        var t = new Other[5];
        print(t.length);
    }
}

// CHECK: 5

//--- Other.java

public class Other
{

}
