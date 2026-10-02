/*
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.fmradio;

import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.LinkedList;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

class AudioRecorder extends HandlerThread implements Handler.Callback {
    public static final int AUDIO_RECORDER_ERROR_INTERNAL = -100;
    public static final int AUDIO_RECORDER_WARN_DISK_LOW = 100;
    private static final boolean TRACE = false;
    private static final String TAG = "AudioRecorder";
    private static final int MSG_INIT = 100;
    private static final int MSG_ENCODE = 101;
    private static final int MSG_STOP = 999;
    private static final long DISK_LOW_THRESHOLD = 10 * 1024 * 1024;
    private AudioFormat mInputFormat;
    private Handler mHandler;
    private File mFilePath;
    private MediaMuxer mMuxer;
    private MediaCodec mCodec;
    private MediaFormat mRequestedFormat;
    private LinkedList<Sample> mQueue = new LinkedList<>();
    private MediaFormat mOutFormat;
    private int mMuxerTrack;
    private double mRate;
    private int mFrameBytes; // bytes per us
    private long mInputBufferPosition;
    private int mInputBufferIndex = -1;
    /** This semaphore is initialized when stopRecording() is called and blocks
        until recording is stopped. */
    private Semaphore mFinalSem;
    private volatile boolean mFinished;
    private boolean mReleased, mMuxerStarted, mInputEos, mDiskWarning;
    private final LinkedList<Integer> mInputs = new LinkedList<>();
    private final AtomicInteger mPendingBytes = new AtomicInteger();
    private final AtomicInteger mPendingError = new AtomicInteger();
    private static final int MAX_PENDING_BYTES = 1024 * 1024;
    private Handler mCallbackHandler;
    private volatile Callback mCallback;

    AudioRecorder(AudioFormat format, File filePath) {
        super("AudioRecorder Thread");
        mFilePath = filePath;
        mInputFormat = format;
        mCallbackHandler = new Handler(Looper.getMainLooper());

        start();

        mHandler = new Handler(getLooper(), this);
        mHandler.obtainMessage(MSG_INIT).sendToTarget();
    }

    public void setCallback(Callback callback) {
        mCallback = callback;
        deliverError(); // initialization may fail before setCallback is called
    }

    /**
     * Encode bytes of audio to file
     *
     * @param bytes - PCM inbut buffer
     */
    public synchronized void encode(byte[] bytes) {
        if (mFinished) {
            Log.w(TAG, "encode() called after stopped");
            return;
        }
        if (bytes.length > MAX_PENDING_BYTES - mPendingBytes.get()) {
            mFinished = true;
            mHandler.post(() -> onError("encoder input backlog exceeded 1 MiB", null));
            return;
        }
        mPendingBytes.addAndGet(bytes.length);
        Sample s = new Sample();
        s.bytes = bytes;
        mHandler.obtainMessage(MSG_ENCODE, s).sendToTarget();
    }

    /**
     * Stop the current recording.
     * Blocks until the recording finishes cleanly.
     */
    public void stopRecording() {
        Semaphore done = new Semaphore(0);
        synchronized (this) {
            if (mFinished) return;
            mFinished = true;
            mHandler.obtainMessage(MSG_STOP, done).sendToTarget();
        }
        try {
            if (!done.tryAcquire(3, TimeUnit.SECONDS)) {
                mHandler.post(() -> onError("encoder EOS timeout", null));
                mPendingError.set(AUDIO_RECORDER_ERROR_INTERNAL);
                deliverError();
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            mHandler.post(() -> onError("encoder stop interrupted", ex));
        }
    }

    private void init() {
        Log.i(TAG, "Starting AudioRecorder with format=" + mInputFormat + ". Saving to: " + mFilePath);
        calculateInputRate();

        mRequestedFormat = new MediaFormat();
        mRequestedFormat.setString(MediaFormat.KEY_MIME, "audio/mp4a-latm");
        mRequestedFormat.setInteger(MediaFormat.KEY_BIT_RATE, 128000);
        mRequestedFormat.setInteger(MediaFormat.KEY_CHANNEL_COUNT, mInputFormat.getChannelCount());
        mRequestedFormat.setInteger(MediaFormat.KEY_SAMPLE_RATE, mInputFormat.getSampleRate());
        mRequestedFormat.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);

        try {
            mCodec = MediaCodec.createEncoderByType("audio/mp4a-latm");
        } catch (IOException ex) {
            onError("failed creating encoder", ex);
            return;
        }
        mCodec.setCallback(new AudioRecorderCodecCallback(), new Handler(getLooper()));
        mCodec.configure(mRequestedFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        mCodec.start();

        try {
            mMuxer = new MediaMuxer(mFilePath.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
        } catch (IOException ex) {
            onError("failed creating muxer", ex);
            return;
        }

        // AAC codec-specific data becomes available in onOutputFormatChanged.
        // Starting the muxer here can create an invalid M4A track without CSD.
    }

    @Override
    public boolean handleMessage(Message msg) {
        if (mReleased) {
            if (msg.what == MSG_STOP) ((Semaphore) msg.obj).release();
            return true;
        }
        try {
            if (msg.what == MSG_INIT) init();
            else if (msg.what == MSG_STOP) { mFinalSem = (Semaphore) msg.obj; drainInputs(); }
            else if (msg.what == MSG_ENCODE) { mQueue.addLast((Sample) msg.obj); drainInputs(); }
        } catch (RuntimeException error) { onError("encoder handler failed", error); }
        return true;
    }

    private void drainInputs() {
        while (!mReleased && !mInputEos && !mInputs.isEmpty()
                && (!mQueue.isEmpty() || mFinalSem != null)) {
            mInputBufferIndex = mInputs.removeFirst();
            processInputBuffer();
        }
    }

    private void processInputBuffer() {
        Sample s = mQueue.peekFirst();
        if (s == null) { // input available?
            if (mFinalSem != null) {
                // input queue is exhausted and stopRecording() is waiting for
                // encoding to finish. signal end-of-stream on the input.
                Log.d(TAG, "Input EOS");
                mCodec.queueInputBuffer(
                        mInputBufferIndex, 0, 0,
                        getPresentationTimestampUs(mInputBufferPosition),
                        MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                mInputEos = true;
                mInputBufferIndex = -1;
            }
            return;
        }

        ByteBuffer b = mCodec.getInputBuffer(mInputBufferIndex);
        if (b == null) throw new IllegalStateException("missing codec input buffer");
        b.clear();
        int sz = Math.min(b.capacity(), s.bytes.length - s.offset);
        sz -= sz % mFrameBytes;
        if (sz <= 0) throw new IllegalStateException("unaligned PCM sample or undersized codec buffer");
        long ts = getPresentationTimestampUs(mInputBufferPosition);
        if (TRACE)
            Log.v(TAG, String.format("processInputBuffer (len=%d) ts=%.3f", sz, ts * 1e-6));

        b.put(s.bytes, s.offset, sz);
        mCodec.queueInputBuffer(mInputBufferIndex, 0, sz, ts, 0);

        mPendingBytes.addAndGet(-sz);
        mInputBufferPosition += sz;
        s.offset += sz;

        // done with this sample?
        if (s.offset >= s.bytes.length) {
            mQueue.pop();
        }

        // done with this buffer
        mInputBufferIndex = -1;
    }

    private void processOutputBuffer(int index, MediaCodec.BufferInfo info) {
        ByteBuffer outputBuffer = mCodec.getOutputBuffer(index);
        if (TRACE)
            Log.v(TAG, String.format("processOutputBuffer (len=%d) ts=%.3f",
                    info.size, info.presentationTimeUs * 1e-6));

        if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
            if (!mMuxerStarted || outputBuffer == null)
                throw new IllegalStateException("AAC output arrived before format");
            outputBuffer.position(info.offset);
            outputBuffer.limit(info.offset + info.size);
            mMuxer.writeSampleData(mMuxerTrack, outputBuffer, info);
        }
        mCodec.releaseOutputBuffer(index, false);
        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
            Log.d(TAG, "Output EOS");
            finish();
        } else if (mFilePath.getFreeSpace() < DISK_LOW_THRESHOLD) {
            onDiskLow();
        }
    }

    private void deliverError() {
        mCallbackHandler.post(() -> {
            Callback callback = mCallback;
            if (callback != null) {
                int error = mPendingError.getAndSet(0);
                if (error != 0) callback.onError(error);
            }
        });
    }

    private void onDiskLow() {
        if (mDiskWarning) return;
        mDiskWarning = true;
        mPendingError.set(AUDIO_RECORDER_WARN_DISK_LOW);
        deliverError();
    }

    private void onError(String message, Exception error) {
        Log.e(TAG, message, error);
        mPendingError.set(AUDIO_RECORDER_ERROR_INTERNAL);
        mFinished = true;
        stopAndRelease();
        deliverError();
    }

    private void finish() { stopAndRelease(); }

    private void stopAndRelease() {
        if (mReleased) return;
        mReleased = true;
        mFinished = true;
        try {
            if (mCodec != null) {
                try { mCodec.stop(); } catch (RuntimeException error) { Log.w(TAG, "codec stop", error); }
                try { mCodec.release(); } catch (RuntimeException error) { Log.w(TAG, "codec release", error); }
                mCodec = null;
            }
            if (mMuxer != null) {
                try {
                    if (mMuxerStarted) mMuxer.stop();
                } catch (RuntimeException error) {
                    mPendingError.set(AUDIO_RECORDER_ERROR_INTERNAL);
                    deliverError();
                    Log.e(TAG, "M4A finalization failed", error);
                }
                try { mMuxer.release(); } catch (RuntimeException error) { Log.w(TAG, "muxer release", error); }
                mMuxer = null;
            }
        } finally {
            mQueue.clear(); mInputs.clear(); mPendingBytes.set(0);
            if (mFinalSem != null) { mFinalSem.release(); mFinalSem = null; }
            quitSafely();
        }
    }

    private void calculateInputRate() {
        int bits_per_sample;
        switch (mInputFormat.getEncoding()) {
            case AudioFormat.ENCODING_PCM_8BIT:
                bits_per_sample = 8;
                break;
            case AudioFormat.ENCODING_PCM_16BIT:
                bits_per_sample = 16;
                break;
            case AudioFormat.ENCODING_PCM_FLOAT:
                bits_per_sample = 32;
                break;
            default:
                throw new IllegalArgumentException("Unexpected encoding: " + mInputFormat.getEncoding());
        }

        mFrameBytes = bits_per_sample / 8 * mInputFormat.getChannelCount();
        mRate = bits_per_sample;
        mRate *= mInputFormat.getSampleRate();
        mRate *= mInputFormat.getChannelCount();
        mRate *= 1e-6; // -> us
        mRate /= 8; // -> bytes

        Log.v(TAG, "Rate: " + mRate);
    }

    private long getPresentationTimestampUs(long position) {
        return (long) (position / mRate);
    }

    public interface Callback {
        void onError(int what);
    }

    class AudioRecorderCodecCallback extends MediaCodec.Callback {

        @Override
        public void onInputBufferAvailable(MediaCodec codec, int index) {
            if (mReleased) return;
            try { mInputs.add(index); drainInputs(); }
            catch (RuntimeException error) { AudioRecorder.this.onError("AAC input failed", error); }
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
            if (mReleased) return;
            try { processOutputBuffer(index, info); }
            catch (RuntimeException error) { AudioRecorder.this.onError("AAC output failed", error); }
        }

        @Override
        public void onError(MediaCodec codec, MediaCodec.CodecException e) {
            if (mReleased) return;
            AudioRecorder.this.onError("Encoder error", e);
        }

        @Override
        public void onOutputFormatChanged(MediaCodec codec, MediaFormat format) {
            if (mReleased) return;
            try {
                if (mMuxerStarted) throw new IllegalStateException("unexpected AAC format change");
                mOutFormat = format;
                mMuxerTrack = mMuxer.addTrack(format);
                mMuxer.start();
                mMuxerStarted = true;
            } catch (RuntimeException error) { AudioRecorder.this.onError("AAC format failed", error); }
        }
    }

    private class Sample {
        byte bytes[];
        int offset;
    }
}
