// SPDX-License-Identifier: Apache-2.0
// Host test fake, never included in the APK.
package android.media;
import java.util.concurrent.atomic.AtomicInteger;
public class AudioRecord {
 public static final int STATE_INITIALIZED=1, RECORDSTATE_RECORDING=3, READ_NON_BLOCKING=1;
 public static volatile AudioRecord last;
 public static volatile AudioDeviceInfo actual=new AudioDeviceInfo(16,42);
 public static volatile int error=0, changeAfter=0;
 public static volatile boolean selectable=true, initialized=true, unsupported=false;
 public final AtomicInteger reads=new AtomicInteger(), releases=new AtomicInteger();
 public static void reset(){last=null;actual=new AudioDeviceInfo(16,42);error=changeAfter=0;selectable=initialized=true;unsupported=false;}
 public static int getMinBufferSize(int rate,int ch,int fmt){return 19200;}
 public int getState(){return initialized?1:0;}
 public boolean setPreferredDevice(AudioDeviceInfo d){assert d.getType()==16;return selectable;}
 public void startRecording(){} public int getRecordingState(){return 3;}
 public AudioFormat getFormat(){return new AudioFormat();}
 public AudioDeviceInfo getRoutedDevice(){assert releases.get()==0;return actual;}
 public int read(byte[] data,int off,int n,int mode){
  assert releases.get()==0 && mode==1;
  try{Thread.sleep(1);}catch(InterruptedException e){Thread.currentThread().interrupt();}
  int count=reads.incrementAndGet(); if(changeAfter>0 && count>=changeAfter)actual=new AudioDeviceInfo(15,43);
  java.util.Arrays.fill(data,(byte)42);return error==0?n:error;
 }
 public void release(){assert releases.incrementAndGet()==1;}
 public static class Builder {
  public Builder setAudioSource(int n){assert n==1998;return this;}
  public Builder setAudioFormat(AudioFormat f){return this;}
  public Builder setBufferSizeInBytes(int n){assert n>=19200;return this;}
  public AudioRecord build(){if(unsupported)throw new UnsupportedOperationException("unsupported format");return last=new AudioRecord();}
 }
}
