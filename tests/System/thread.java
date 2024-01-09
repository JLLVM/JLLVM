// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class

class Test
{
    public static native void print(long l);
    public static native void print(boolean b);

    public static void main(String[] args) throws InterruptedException
    {
        var t = Thread.currentThread();
        // CHECK: 1
        print(t != null);

        // CHECK: 1
        print(t.getState() == Thread.State.RUNNABLE);

        // CHECK: 1
        print(t.isAlive());

        Thread.yield();

        Thread.sleep(1000);

        t = new Thread();

        // CHECK: 0
        print(t.isAlive());
        t.start();

        // CHECK: 1
        print(t.isAlive());

        var o = new Test();

        synchronized (o)
        {
            // CHECK: 1
            print(Thread.holdsLock(o));
        }


    }
}
