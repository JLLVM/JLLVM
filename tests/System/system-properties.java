// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

class Test
{
    public static native void print(String s);
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        // CHECK: 1
        print(System.getProperty("java.io.tmpdir") != null);
        // CHECK: 1
        print(System.getProperty("java.home") != null);
        var tmp = System.getProperty("file.separator");
        // CHECK: 1
        print(tmp == "/" || tmp == "\\");
        tmp = System.getProperty("line.separator");
        // CHECK: 1
        print(tmp == "\n" || tmp == "\n\r");
        tmp = System.getProperty("path.separator");
        // CHECK: 1
        print(tmp == ":" || tmp == ";");
        // CHECK: 1
        print(System.getProperty("user.dir") != null);
        // CHECK: 1
        print(System.getProperty("user.home") != null);
        // CHECK: 1
        print(System.getProperty("file.encoding") == "UTF-8");
        // CHECK: 1
        print(System.getProperty("native.encoding") == "UTF-8");


        // CHECK: 1
        print(System.getProperty("unknown that really should not exist") == null);
    }
}
