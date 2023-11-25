// RUN: rm -rf %t && split-file %s %t
// RUN: cd %t && javac %t/Test.java -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

//--- Test.java

public class Test
{
    public static native void print(String i);

    public static void main(String[] args)
    {
        Object o = new Test();
        var t = (Test)o;
        try
        {
            var z = (Other)o;
        }
        catch (ClassCastException e)
        {
            // CHECK: Caught
            print("Caught");
        }
        o = null;
        var z = (Other)o;
        // CHECK: Finished without exceptions
        print("Finished without exceptions");
    }
}

//--- Other.java

public class Other
{

}
