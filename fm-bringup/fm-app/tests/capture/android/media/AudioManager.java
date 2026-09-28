// SPDX-License-Identifier: Apache-2.0
// Host test fake, never included in the APK.
package android.media;
public class AudioManager {
 public static final int MODE_NORMAL=0, GET_DEVICES_INPUTS=1;
 public int mode=0; public boolean sco=false;
 public AudioDeviceInfo[] inputs={new AudioDeviceInfo(16,42)};
 public int getMode(){return mode;} public boolean isBluetoothScoOn(){return sco;}
 public AudioDeviceInfo[] getDevices(int flag){return inputs;}
}
