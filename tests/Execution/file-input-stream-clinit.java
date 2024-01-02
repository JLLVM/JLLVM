// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class

import java.io.FileInputStream;

class Test
{
    public static void main(String[] args) throws ClassNotFoundException
    {
        var clazz = FileInputStream.class;
        Class.forName(clazz.getName(), /*initialize=*/true, clazz.getClassLoader());
    }
}
