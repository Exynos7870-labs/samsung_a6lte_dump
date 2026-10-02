// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.media;
public class MediaFormat {
 public static final String KEY_MIME="mime",KEY_BIT_RATE="bitrate",KEY_CHANNEL_COUNT="channels",KEY_SAMPLE_RATE="rate",KEY_AAC_PROFILE="aac";
 public boolean csd;
 public void setString(String k,String v){} public void setInteger(String k,int v){}
}
