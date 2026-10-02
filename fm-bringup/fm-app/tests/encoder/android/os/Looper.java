// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.os;
import java.util.concurrent.*;
public class Looper {
 final BlockingQueue<Runnable> queue=new LinkedBlockingQueue<>();
 volatile boolean quitting; public static volatile Throwable failure;
 private static final Looper MAIN=new Looper();
 static {Thread t=new Thread(()->MAIN.loop());t.setDaemon(true);t.start();}
 public static Looper getMainLooper(){return MAIN;}
 void loop(){try{while(!quitting || !queue.isEmpty()){Runnable r=queue.poll(50,TimeUnit.MILLISECONDS);if(r!=null)r.run();}}
 catch(Throwable e){failure=e;quitting=true;}}
}
