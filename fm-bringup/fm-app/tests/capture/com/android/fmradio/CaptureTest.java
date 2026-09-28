// SPDX-License-Identifier: Apache-2.0
package com.android.fmradio;
import android.media.*;
import android.os.SystemClock;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
public class CaptureTest {
    static void waitFor(BooleanSupplier done) {
        long end=SystemClock.elapsedRealtime()+2000;
        while(!done.getAsBoolean() && SystemClock.elapsedRealtime()<end)SystemClock.sleep(1);
        assert done.getAsBoolean();
    }
    public static void main(String[] args) {
        AudioManager manager=new AudioManager();
        AudioRecord.reset(); manager.inputs=new AudioDeviceInfo[0];
        assert BcmFmCapture.open(manager)==null && AudioRecord.last==null;
        System.out.println("PASS capture requires an exposed FM input");
        manager=new AudioManager(); manager.mode=2;
        assert BcmFmCapture.open(manager)==null;
        manager.mode=0;manager.sco=true;assert BcmFmCapture.open(manager)==null;
        System.out.println("PASS capture refuses call/SCO modes");
        manager=new AudioManager();
        AudioRecord.reset();AudioRecord.actual=new AudioDeviceInfo(15,43);
        assert BcmFmCapture.open(manager)==null && AudioRecord.last.releases.get()==1;
        System.out.println("PASS capture rejects microphone fallback");
        AudioRecord.reset();AudioRecord.actual=new AudioDeviceInfo(16,99);
        assert BcmFmCapture.open(manager)==null && AudioRecord.last.releases.get()==1;
        System.out.println("PASS capture requires the selected FM device ID");
        AudioRecord.reset();AudioRecord.selectable=false;
        assert BcmFmCapture.open(manager)==null && AudioRecord.last.releases.get()==1;
        AudioRecord.reset();AudioRecord.initialized=false;
        assert BcmFmCapture.open(manager)==null && AudioRecord.last.releases.get()==1;
        AudioRecord.reset();AudioRecord.error=-6;
        assert BcmFmCapture.open(manager)==null && AudioRecord.last.releases.get()==1;
        AudioRecord.reset();AudioRecord.unsupported=true;
        assert BcmFmCapture.open(manager)==null && AudioRecord.last==null;
        System.out.println("PASS capture construction/read failures release resources");
        AudioRecord.reset();BcmFmCapture unused=BcmFmCapture.open(manager);
        assert unused!=null;unused.stop();unused.stop();assert AudioRecord.last.releases.get()==1;
        System.out.println("PASS prepared capture stop is idempotent");
        AudioRecord.reset();BcmFmCapture capture=BcmFmCapture.open(manager);
        AtomicInteger frames=new AtomicInteger(),errors=new AtomicInteger();
        BcmFmCapture.Sink sink=new BcmFmCapture.Sink(){
            public void pcm(byte[] data){assert data.length%4==0 && data[0]==42;frames.incrementAndGet();}
            public void failed(){errors.incrementAndGet();}
        };
        assert capture.start(sink);waitFor(()->frames.get()>2);capture.stop();
        int atStop=frames.get();waitFor(()->AudioRecord.last.releases.get()==1);
        assert frames.get()==atStop && errors.get()==0 && !capture.start(sink);
        System.out.println("PASS real capture loop feeds PCM and stops without late frames/double release");
        AudioRecord.reset();AudioRecord.changeAfter=2;frames.set(0);errors.set(0);
        capture=BcmFmCapture.open(manager);assert capture!=null;assert capture.start(sink);
        waitFor(()->errors.get()==1);assert frames.get()==0;capture.stop();
        assert AudioRecord.last.releases.get()==1;
        System.out.println("PASS route switch during a read discards data and signals failure");
        System.out.println("8 capture tests passed (mock Android audio, not hardware)");
    }
}
