package org.nx2a;

import android.app.Activity;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.InputDevice;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A controller's own gyroscope and accelerometer, feeding the host's six-axis sensor (front_android.c, Java_org_nx2a_PadMotion_nativeSample → dm_motion_pad_set).
 *
 * A Bluetooth pad arrives as an ordinary InputDevice: its buttons and sticks work, and its motion used to go nowhere. Android 12 (API 31) exposes the sensors of one input
 * device: InputDevice.getSensorManager() hands back a SensorManager scoped to that controller, carrying its TYPE_GYROSCOPE and TYPE_ACCELEROMETER. It is the only way an
 * app reaches them (the NDK has no per-device SensorManager), and the APK is otherwise a code-less NativeActivity: this class is the APK's classes.dex, built by the porter
 * when Android's SDK is there (porter/pipeline.py) and left out when it is not (the host then logs that the pad's motion is not read).
 *
 * Samples ride one HandlerThread, never the main thread, and batching is off (maxReportLatencyUs = 0): batching trades away exactly the latency motion aiming exists to
 * avoid. 200 Hz is the rate the device's own path asks for (the Switch's sensor rate), and the ceiling the framework grants an app without HIGH_SAMPLING_RATE_SENSORS.
 *
 * A feed that lets go of a pad parks its rotation at zero first: the host holds motion as state (hle_hid.c) and re-emits it in every sample, so an angular velocity left
 * behind would read as a pad still turning. The device's own gyroscope has the same rule (front_android.c).
 *
 * Frame and units: the values go to the host as Android reports them (rad/s and m/s²). A controller's sensors are in Android's own frame for the device (x to its right,
 * y up, z out of its face), which is the Switch controller's frame once the face is read as the player's screen, and unlike the device's motion it does not turn with the
 * display. That mapping is the documentation's and not a measurement on hardware: front_android.c logs each pad's first sample, and a pad that comes through permuted or
 * negated needs its remap there.
 */
public final class PadMotion implements InputManager.InputDeviceListener {
    private static final String TAG = "nx2a";
    private static final int RATE_US = 5000;   // 200 Hz, unbatched
    private static final float G = 9.80665f;

    private static PadMotion sInstance;

    private final InputManager mInput;
    private final HandlerThread mThread;
    private final Handler mHandler;
    private final ConcurrentHashMap<Integer, Feed> mFeeds = new ConcurrentHashMap<>();

    /** One controller's sensors: the listener, and the acceleration that pairs with each rotation sample. */
    private final class Feed implements SensorEventListener {
        private final int mId;
        private final SensorManager mManager;
        private final float[] mAcc = {0f, 0f, G};   // gravity on the up axis at rest, not free fall (the host's neutral)

        Feed(int id, SensorManager manager) {
            mId = id;
            mManager = manager;
        }

        @Override
        public void onSensorChanged(SensorEvent e) {
            if (e.values.length < 3) return;
            int type = e.sensor.getType();
            if (type == Sensor.TYPE_ACCELEROMETER) {
                mAcc[0] = e.values[0];
                mAcc[1] = e.values[1];
                mAcc[2] = e.values[2];
            } else if (type == Sensor.TYPE_GYROSCOPE) {
                nativeSample(mId, e.values[0], e.values[1], e.values[2], mAcc[0], mAcc[1], mAcc[2], e.timestamp);
            }
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    }

    private PadMotion(Activity activity) {
        mInput = activity.getSystemService(InputManager.class);
        mThread = new HandlerThread("nx2a-pad-motion");
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        if (mInput != null) mInput.registerInputDeviceListener(this, mHandler);
        for (int id : InputDevice.getDeviceIds()) attach(id);
    }

    /** Called once by the host's input thread with the running activity (front_android.c, pad_motion_start); a no-op twice, and below Android 12. */
    public static void start(Activity activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            Log.i(TAG, "pad motion: Android 12 or newer is needed to read a controller's sensors");
            return;
        }
        if (sInstance != null) return;
        sInstance = new PadMotion(activity);
    }

    @Override
    public void onInputDeviceAdded(int id) {
        attach(id);
    }

    @Override
    public void onInputDeviceChanged(int id) {
        attach(id);
    }

    @Override
    public void onInputDeviceRemoved(int id) {
        detach(id);
    }

    /** Register this pad's sensors when it is a controller with a gyroscope; a pad without one leaves the device's own gyroscope speaking. */
    private void attach(int id) {
        if (mFeeds.containsKey(id)) return;
        InputDevice dev = InputDevice.getDevice(id);
        if (dev == null || dev.isVirtual()) return;
        int src = dev.getSources();
        if ((src & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD && (src & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return;
        SensorManager sm = dev.getSensorManager();
        Sensor gyro = sm == null ? null : sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        if (gyro == null) return;   // a pad with no gyroscope: nothing to add
        Feed f = new Feed(id, sm);
        mFeeds.put(id, f);
        sm.registerListener(f, gyro, RATE_US, 0, mHandler);
        Sensor accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (accel != null) sm.registerListener(f, accel, RATE_US, 0, mHandler);
        Log.i(TAG, "pad motion: " + dev.getName() + " (" + id + ") has a gyroscope");
    }

    /** A pad unplugged, unpaired or switched off: stop listening, and park the rotation the host is holding. */
    private void detach(int id) {
        Feed f = mFeeds.remove(id);
        if (f == null) return;
        f.mManager.unregisterListener(f);
        nativeSample(id, 0f, 0f, 0f, f.mAcc[0], f.mAcc[1], f.mAcc[2], System.nanoTime());
    }

    /** One gyroscope (rad/s) and accelerometer (m/s²) sample, with the sensor clock: front_android.c's Java_org_nx2a_PadMotion_nativeSample. */
    private static native void nativeSample(int id, float gx, float gy, float gz, float ax, float ay, float az, long tNs);
}
