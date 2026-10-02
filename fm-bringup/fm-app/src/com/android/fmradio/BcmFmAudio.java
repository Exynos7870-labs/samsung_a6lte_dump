/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The LineageOS Project
 */
package com.android.fmradio;

import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

/** Experimental SEC HAL direct-FM route; no raw mixer or PCM device access. */
final class BcmFmAudio {
    private static final String TAG = "BcmFmAudio";
    private final AudioManager mManager;
    private AudioTrack mTrack;
    private boolean mSpeaker;

    BcmFmAudio(AudioManager manager) {
        mManager = manager;
        // Also recover the HAL route after a previous app process died.
        mManager.setParameters("l_fmradio_mode=off");
    }

    private AudioDeviceInfo output(boolean speaker) {
        for (AudioDeviceInfo device : mManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            int type = device.getType();
            if ((speaker && type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER)
                    || (!speaker && (type == AudioDeviceInfo.TYPE_WIRED_HEADSET
                    || type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES))) {
                return device;
            }
        }
        return null;
    }

    synchronized boolean start(boolean speaker) {
        if (mTrack != null) return selectSpeaker(speaker);
        if (mManager.getMode() != AudioManager.MODE_NORMAL || mManager.isBluetoothScoOn()) {
            return false;
        }
        AudioDeviceInfo device = output(speaker);
        if (device == null) return false;
        try {
            mManager.setParameters("l_fmradio_mode=ready");
            // Keep a primary MUSIC output active so the SEC HAL selects the FM
            // mixer route. The real audio comes from the combo chip via FM DAI 4,
            // not from these zero samples. Do not software-loop FM on top of it.
            mTrack = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                    .setTransferMode(AudioTrack.MODE_STATIC)
                    .setBufferSizeInBytes(19200).build();
            if (mTrack.getState() == AudioTrack.STATE_UNINITIALIZED
                    || !mTrack.setPreferredDevice(device)) throw new IllegalStateException("no FM output");
            byte[] silence = new byte[19200];
            if (mTrack.write(silence, 0, silence.length) != silence.length
                    || mTrack.setLoopPoints(0, silence.length / 4, -1) != AudioTrack.SUCCESS) {
                throw new IllegalStateException("cannot initialize FM keep-alive track");
            }
            mSpeaker = speaker;
            mTrack.play();
            mManager.setParameters("l_fmradio_mode=on");
            return true;
        } catch (RuntimeException error) {
            Log.e(TAG, "FM audio start failed", error);
            stop();
            return false;
        }
    }

    synchronized boolean selectSpeaker(boolean speaker) {
        mSpeaker = speaker;
        if (mTrack == null) return true;
        AudioDeviceInfo device = output(speaker);
        return device != null && mTrack.setPreferredDevice(device);
    }

    synchronized int digitalVolume() {
        int index = mManager.getStreamVolume(AudioManager.STREAM_MUSIC);
        if (index == 0 || mManager.isStreamMute(AudioManager.STREAM_MUSIC)) return 0;
        int type = mSpeaker ? AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
                : AudioDeviceInfo.TYPE_WIRED_HEADPHONES;
        float db = mManager.getStreamVolumeDb(AudioManager.STREAM_MUSIC, index, type);
        // Direct HAL FM playback bypasses AudioTrack's sample-volume scaling.
        // Follow the MUSIC attenuation curve in the receiver rather than playing
        // full-volume radio while changing only the gain of the silence track.
        return Math.max(0, Math.min(255, Math.round(255.0f * (float) Math.pow(10.0, db / 20.0))));
    }

    synchronized void stop() {
        // Turn off the SEC route before releasing the track/going into standby.
        mManager.setParameters("l_fmradio_mode=off");
        if (mTrack != null) {
            try { mTrack.stop(); } catch (IllegalStateException ignored) { }
            mTrack.release();
            mTrack = null;
        }
    }
}
