// SPDX-License-Identifier: Apache-2.0
// Host test fake, never included in the APK.
package android.media;
public class AudioFormat {
 public static final int ENCODING_PCM_16BIT=2, ENCODING_PCM_8BIT=3, ENCODING_PCM_FLOAT=4, CHANNEL_IN_STEREO=12;
 public int getEncoding(){return 2;} public int getChannelCount(){return 2;} public int getSampleRate(){return 48000;}
 public static class Builder {
  public Builder setSampleRate(int n){assert n==48000; return this;}
  public Builder setEncoding(int n){assert n==2;return this;}
  public Builder setChannelMask(int n){assert n==12;return this;}
  public AudioFormat build(){return new AudioFormat();}
 }
}
