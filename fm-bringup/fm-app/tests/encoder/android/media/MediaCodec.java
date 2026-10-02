// SPDX-License-Identifier: Apache-2.0
// Host-only fake, not an Android codec test.
package android.media;
import java.io.*;import java.nio.*;import java.util.*;import android.os.Handler;
public class MediaCodec {
 public static final int CONFIGURE_FLAG_ENCODE=1,BUFFER_FLAG_END_OF_STREAM=4,BUFFER_FLAG_CODEC_CONFIG=2;
 public static volatile int mode, attempts;public static volatile MediaCodec last;
 public volatile int releases,bytes,eos;public final Set<Integer> used=Collections.synchronizedSet(new HashSet<>());
 private Callback callback;private Handler handler;
 public static void reset(){mode=attempts=0;last=null;MediaMuxer.last=null;MediaMuxer.failStop=false;}
 public static MediaCodec createEncoderByType(String mime)throws IOException{attempts++;if(mode==1)throw new IOException("init");return last=new MediaCodec();}
 public void setCallback(Callback c,Handler h){callback=c;handler=h;}
 public void configure(MediaFormat f,Object surface,Object crypto,int flags){}
 public void start(){
  handler.post(()->{MediaFormat f=new MediaFormat();f.csd=true;callback.onOutputFormatChanged(this,f);});
  handler.post(()->callback.onOutputBufferAvailable(this,0,new BufferInfo(0,4,BUFFER_FLAG_CODEC_CONFIG)));
  if(mode!=2)for(int i=0;i<4;i++){final int index=i;handler.post(()->callback.onInputBufferAvailable(this,index));}
 }
 public MediaFormat getOutputFormat(){throw new IllegalStateException("format not ready");}
 public ByteBuffer getInputBuffer(int i){return ByteBuffer.allocate(1024);}
 public ByteBuffer getOutputBuffer(int i){return ByteBuffer.allocate(32);}
 public void queueInputBuffer(int index,int offset,int size,long pts,int flags){
  used.add(index);bytes+=size;
  if((flags&BUFFER_FLAG_END_OF_STREAM)!=0){eos++;handler.post(()->callback.onOutputBufferAvailable(this,index,new BufferInfo(0,0,BUFFER_FLAG_END_OF_STREAM)));}
  else {handler.post(()->callback.onOutputBufferAvailable(this,index,new BufferInfo(2,3,0)));
        handler.post(()->callback.onInputBufferAvailable(this,index));}
 }
 public void releaseOutputBuffer(int i,boolean render){}
 public void stop(){}public void release(){assert ++releases==1;}
 public static class BufferInfo {public int size,offset,flags;public long presentationTimeUs;public BufferInfo(int o,int s,int f){offset=o;size=s;flags=f;}}
 public static class CodecException extends RuntimeException {private static final long serialVersionUID=1;}
 public abstract static class Callback {
  public abstract void onInputBufferAvailable(MediaCodec c,int index);
  public abstract void onOutputBufferAvailable(MediaCodec c,int index,BufferInfo info);
  public abstract void onOutputFormatChanged(MediaCodec c,MediaFormat format);
  public abstract void onError(MediaCodec c,CodecException error);
 }
}
