// SPDX-License-Identifier: Apache-2.0
// Host test fake, never included in the APK.
package android.media;
public class AudioDeviceInfo {
 public static final int TYPE_FM_TUNER=16, TYPE_BUILTIN_MIC=15;
 private int type,id; public AudioDeviceInfo(int t,int i){type=t;id=i;}
 public int getType(){return type;} public int getId(){return id;}
}
