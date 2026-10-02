/* SPDX-License-Identifier: Apache-2.0 */
package com.android.fmradio;

import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.SystemClock;
import android.util.Log;
import java.util.Arrays;

/** Capture ONLY the FM tuner. Never fall back to microphone, voice call or mix. */
final class BcmFmCapture {
    interface Sink { void pcm(byte[] samples); void failed(); }
    private static final String TAG = "BcmFmCapture";
    private final AudioRecord mRecord;
    private final int mDeviceId;
    private volatile boolean mClosed;
    private Thread mThread;

    private BcmFmCapture(AudioRecord record, int deviceId) {
        mRecord = record;
        mDeviceId = deviceId;
    }

    static BcmFmCapture open(AudioManager manager) {
        AudioRecord record = null;
        try {
            if (manager.getMode() != AudioManager.MODE_NORMAL || manager.isBluetoothScoOn()) return null;
            AudioDeviceInfo fm = null;
            for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                if (device.getType() == AudioDeviceInfo.TYPE_FM_TUNER) { fm = device; break; }
            }
            if (fm == null) throw new IllegalStateException("FM tuner input is not exposed by audio policy");
            AudioFormat format = new AudioFormat.Builder().setSampleRate(48000)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build();
            int minimum = AudioRecord.getMinBufferSize(48000,
                    AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new IllegalStateException("unsupported FM capture format");
            record = new AudioRecord.Builder().setAudioSource(MediaRecorder.AudioSource.RADIO_TUNER)
                    .setAudioFormat(format).setBufferSizeInBytes(Math.max(minimum, 19200)).build();
            if (record.getState() != AudioRecord.STATE_INITIALIZED || !record.setPreferredDevice(fm))
                throw new IllegalStateException("cannot select FM capture device");
            record.startRecording();
            if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING)
                throw new IllegalStateException("FM capture did not start");
            BcmFmCapture capture = new BcmFmCapture(record, fm.getId());
            byte[] discard = new byte[3840];
            long deadline = SystemClock.elapsedRealtime() + 1000;
            // Routing is asynchronous. Discard startup data; create no recording
            // until the actual route (not merely the preferred device) is FM.
            while (SystemClock.elapsedRealtime() < deadline) {
                int n = record.read(discard, 0, discard.length, AudioRecord.READ_NON_BLOCKING);
                if (n < 0) throw new IllegalStateException("FM capture read error " + n);
                if (n > 0 && capture.onFm()) return capture;
                AudioDeviceInfo routed = record.getRoutedDevice();
                if (routed != null && !capture.onFm())
                    throw new IllegalStateException("refusing non-FM input");
                SystemClock.sleep(20);
            }
            throw new IllegalStateException("FM capture routing timeout");
        } catch (RuntimeException error) {
            Log.e(TAG, "FM capture unavailable", error);
            if (record != null) record.release();
            return null;
        }
    }

    AudioFormat format() { return mRecord.getFormat(); }
    private boolean onFm() {
        AudioDeviceInfo device = mRecord.getRoutedDevice();
        return device != null && device.getType() == AudioDeviceInfo.TYPE_FM_TUNER
                && device.getId() == mDeviceId;
    }
    synchronized boolean start(Sink sink) {
        if (mClosed || mThread != null) return false;
        mThread = new Thread(() -> {
            boolean failed = false;
            try {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);
                byte[] buffer = new byte[3840]; // 20 ms of stereo PCM16
                long lastData = SystemClock.elapsedRealtime();
                while (!mClosed) {
                    if (!onFm()) throw new IllegalStateException("FM input route changed");
                    int n = mRecord.read(buffer, 0, buffer.length, AudioRecord.READ_NON_BLOCKING);
                    if (n < 0 || n % 4 != 0) throw new IllegalStateException("FM capture failed " + n);
                    if (!onFm()) throw new IllegalStateException("FM input changed during read");
                    if (n > 0) {
                        lastData = SystemClock.elapsedRealtime();
                        synchronized (BcmFmCapture.this) {
                            if (!mClosed) sink.pcm(Arrays.copyOf(buffer, n));
                        }
                    } else {
                        if (SystemClock.elapsedRealtime() - lastData > 2000)
                            throw new IllegalStateException("FM input stalled");
                        SystemClock.sleep(10);
                    }
                }
            } catch (RuntimeException error) {
                failed = !mClosed;
                if (failed) Log.e(TAG, "Stopping FM capture", error);
            } finally {
                mRecord.release(); // sole owner after thread starts
                if (failed) sink.failed();
            }
        }, "FM tuner capture");
        mThread.start();
        return true;
    }
    synchronized void stop() {
        if (mClosed) return;
        mClosed = true;
        // Nonblocking reader exits without waiting for the encoder or a read.
        // Only the worker releases a running AudioRecord, avoiding use-after-release.
        if (mThread == null) mRecord.release();
    }
}
