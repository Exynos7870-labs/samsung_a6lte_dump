// SPDX-License-Identifier: Apache-2.0
// Host test fake, never included in the APK.
package android.os;
public class SystemClock {
 public static long elapsedRealtime(){return System.nanoTime()/1000000;}
 public static void sleep(long ms){try{Thread.sleep(ms);}catch(InterruptedException e){Thread.currentThread().interrupt();}}
}
