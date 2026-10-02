// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.os;
public class HandlerThread extends Thread {
 private final Looper looper=new Looper();
 public HandlerThread(String name){super(name);setDaemon(true);}
 public Looper getLooper(){return looper;}
 public void run(){looper.loop();}
 public boolean quitSafely(){looper.quitting=true;return true;}
}
