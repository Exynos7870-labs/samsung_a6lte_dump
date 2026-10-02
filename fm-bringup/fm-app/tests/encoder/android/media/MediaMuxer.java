// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.media;
import java.io.*;import java.nio.*;
public class MediaMuxer {
 public static class OutputFormat {public static final int MUXER_OUTPUT_MPEG_4=0;}
 public static volatile MediaMuxer last;public static volatile boolean failStop;
 public volatile boolean started;public volatile int samples,releases;
 public MediaMuxer(String name,int format)throws IOException{new File(name).createNewFile();last=this;}
 public int addTrack(MediaFormat format){assert format.csd;return 0;}
 public void start(){assert !started;started=true;}
 public void writeSampleData(int track,ByteBuffer data,MediaCodec.BufferInfo info){
  assert started && info.size>0 && (info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)==0;
  assert data.position()==info.offset && data.limit()==info.offset+info.size;samples++;
 }
 public void stop(){assert started;if(failStop)throw new IllegalStateException("bad finalization");started=false;}
 public void release(){assert ++releases==1;}
}
