// SPDX-License-Identifier: Apache-2.0
package com.android.fmradio;
import android.media.*;
import android.os.*;
import java.io.File;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
public class EncoderTest {
    static void waitFor(BooleanSupplier done) {
        long end=SystemClock.elapsedRealtime()+2000;
        while(!done.getAsBoolean() && SystemClock.elapsedRealtime()<end){
            if(Looper.failure!=null)throw new AssertionError(Looper.failure);
            SystemClock.sleep(1);
        }
        if(Looper.failure!=null)throw new AssertionError(Looper.failure);
        assert done.getAsBoolean();
    }
    static AudioRecorder create() throws Exception {
        File file=File.createTempFile("fm-encoder-", ".m4a");file.deleteOnExit();
        return new AudioRecorder(new AudioFormat.Builder().build(),file);
    }
    public static void main(String[] args)throws Exception {
        AtomicInteger errors=new AtomicInteger();
        MediaCodec.reset();AudioRecorder r=create();r.setCallback(e->errors.incrementAndGet());
        r.encode(new byte[3840]);r.stopRecording();waitFor(()->MediaCodec.last.releases==1);
        assert MediaCodec.last.bytes==3840 && MediaCodec.last.eos==1 && MediaCodec.last.used.size()>1;
        assert MediaMuxer.last.samples==4 && MediaMuxer.last.releases==1 && errors.get()==0;
        r.stopRecording();r.encode(new byte[16]);assert MediaCodec.last.bytes==3840;
        System.out.println("PASS encoder waits for CSD, skips config, handles all input buffers and drains one EOS");
        MediaCodec.reset();errors.set(0);MediaCodec.mode=1;r=create();
        waitFor(()->MediaCodec.attempts==1);SystemClock.sleep(30);
        r.setCallback(e->errors.incrementAndGet());waitFor(()->errors.get()==1);r.stopRecording();
        System.out.println("PASS encoder early initialization error reaches a late callback");
        MediaCodec.reset();errors.set(0);MediaCodec.mode=2;r=create();r.setCallback(e->errors.incrementAndGet());
        r.encode(new byte[1024*1024+1]);waitFor(()->errors.get()==1);r.stopRecording();
        assert MediaCodec.last.releases==1 && MediaCodec.last.bytes==0;
        System.out.println("PASS encoder bounds stalled PCM backlog");
        MediaCodec.reset();errors.set(0);MediaCodec.mode=2;r=create();r.setCallback(e->errors.incrementAndGet());
        long start=SystemClock.elapsedRealtime();r.stopRecording();
        assert SystemClock.elapsedRealtime()-start<5500;waitFor(()->MediaCodec.last.releases==1);
        waitFor(()->errors.get()>0);
        System.out.println("PASS encoder EOS timeout is bounded and cleans up");
        MediaCodec.reset();errors.set(0);MediaMuxer.failStop=true;r=create();r.setCallback(e->errors.incrementAndGet());
        r.encode(new byte[3840]);r.stopRecording();waitFor(()->errors.get()==1);
        assert MediaCodec.last.releases==1 && MediaMuxer.last.releases==1;
        System.out.println("PASS encoder finalization failure reports an error and releases once");
        MediaCodec.reset();errors.set(0);r=create();r.setCallback(e->errors.incrementAndGet());
        r.encode(new byte[3]);waitFor(()->errors.get()==1);r.stopRecording();
        assert MediaCodec.last.releases==1;
        System.out.println("PASS encoder rejects unaligned PCM without a worker crash");
        System.out.println("6 encoder tests passed (mock codec/muxer, not M4A validation)");
    }
}
