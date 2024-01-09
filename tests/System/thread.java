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

        Thread.yield();

        Thread.sleep(1000);

        new Thread().start();

        var o = new Test();

        synchronized (o)
        {
            // CHECK: 1
            print(Thread.holdsLock(o));
        }


    }
}
