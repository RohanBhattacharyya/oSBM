package org.libsdl.app;


import android.content.Context;
import android.content.pm.ActivityInfo;
import android.graphics.Insets;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;


/**
    SDLSurface. This is what we draw on, so we need to know when it's created
    in order to do anything useful.

    Because of this, that's where we set up the SDL thread
*/
public class SDLSurface extends SurfaceView implements SurfaceHolder.Callback,
    View.OnApplyWindowInsetsListener, View.OnKeyListener, View.OnTouchListener, SensorEventListener  {

    // Sensors
    protected SensorManager mSensorManager;
    protected Display mDisplay;
    protected boolean mGyroscopeEnabled;
    protected Sensor mGyroscopeSensor;
    protected int mGyroscopeSensorType;
    protected boolean mAccelerometerGyroFallbackEnabled;
    protected boolean mHasLastRotationQuaternion;
    protected final float[] mLastRotationQuaternion = new float[4];
    protected long mLastRotationVectorTimestamp;
    protected boolean mHasLastGravityVector;
    protected final float[] mLastGravityVector = new float[3];
    protected long mLastGravityTimestamp;
    protected boolean mHasFilteredAccelerometer;
    protected final float[] mFilteredAccelerometer = new float[3];
    protected long mLastNonAccelerometerGyroTimestamp;

    // Keep track of the surface size to normalize touch events
    protected float mWidth, mHeight;

    // Is SurfaceView ready for rendering
    public boolean mIsSurfaceReady;

    // Startup
    public SDLSurface(Context context) {
        super(context);
        getHolder().addCallback(this);

        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
        setOnApplyWindowInsetsListener(this);
        setOnKeyListener(this);
        setOnTouchListener(this);

        mDisplay = ((WindowManager)context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
        mSensorManager = (SensorManager)context.getSystemService(Context.SENSOR_SERVICE);

        setOnGenericMotionListener(SDLActivity.getMotionListener());

        // Some arbitrary defaults to avoid a potential division by zero
        mWidth = 1.0f;
        mHeight = 1.0f;

        mIsSurfaceReady = false;
    }

    public void handlePause() {
        enableSensor(Sensor.TYPE_ACCELEROMETER, false);
        boolean gyroscopeEnabled = mGyroscopeEnabled;
        enableSensor(Sensor.TYPE_GYROSCOPE, false);
        mGyroscopeEnabled = gyroscopeEnabled;
    }

    public void handleResume() {
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
        setOnApplyWindowInsetsListener(this);
        setOnKeyListener(this);
        setOnTouchListener(this);
        enableSensor(Sensor.TYPE_ACCELEROMETER, true);
        if (mGyroscopeEnabled) {
            enableSensor(Sensor.TYPE_GYROSCOPE, true);
        }
    }

    public Surface getNativeSurface() {
        return getHolder().getSurface();
    }

    // Called when we have a valid drawing surface
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.v("SDL", "surfaceCreated()");
        SDLActivity.onNativeSurfaceCreated();
    }

    // Called when we lose the surface
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.v("SDL", "surfaceDestroyed()");

        // Transition to pause, if needed
        SDLActivity.mNextNativeState = SDLActivity.NativeState.PAUSED;
        SDLActivity.handleNativeState();

        mIsSurfaceReady = false;
        SDLActivity.onNativeSurfaceDestroyed();
    }

    // Called when the surface is resized
    @Override
    public void surfaceChanged(SurfaceHolder holder,
                               int format, int width, int height) {
        Log.v("SDL", "surfaceChanged()");

        if (SDLActivity.mSingleton == null) {
            return;
        }

        mWidth = width;
        mHeight = height;
        int nDeviceWidth = width;
        int nDeviceHeight = height;
        float density = 1.0f;
        try
        {
            if (Build.VERSION.SDK_INT >= 17 /* Android 4.2 (JELLY_BEAN_MR1) */) {
                DisplayMetrics realMetrics = new DisplayMetrics();
                mDisplay.getRealMetrics( realMetrics );
                nDeviceWidth = realMetrics.widthPixels;
                nDeviceHeight = realMetrics.heightPixels;
                // Use densityDpi instead of density to more closely match what the UI scale is
                density = (float)realMetrics.densityDpi / 160.0f;
            }
        } catch(Exception ignored) {
        }

        synchronized(SDLActivity.getContext()) {
            // In case we're waiting on a size change after going fullscreen, send a notification.
            SDLActivity.getContext().notifyAll();
        }

        Log.v("SDL", "Window size: " + width + "x" + height);
        Log.v("SDL", "Device size: " + nDeviceWidth + "x" + nDeviceHeight);
        SDLActivity.nativeSetScreenResolution(width, height, nDeviceWidth, nDeviceHeight, density, mDisplay.getRefreshRate());
        SDLActivity.onNativeResize();

        // Prevent a screen distortion glitch,
        // for instance when the device is in Landscape and a Portrait App is resumed.
        boolean skip = false;
        int requestedOrientation = SDLActivity.mSingleton.getRequestedOrientation();

        if (requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_PORTRAIT || requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT) {
            if (mWidth > mHeight) {
               skip = true;
            }
        } else if (requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE || requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE) {
            if (mWidth < mHeight) {
               skip = true;
            }
        }

        // Special Patch for Square Resolution: Black Berry Passport
        if (skip) {
           double min = Math.min(mWidth, mHeight);
           double max = Math.max(mWidth, mHeight);

           if (max / min < 1.20) {
              Log.v("SDL", "Don't skip on such aspect-ratio. Could be a square resolution.");
              skip = false;
           }
        }

        // Don't skip if we might be multi-window or have popup dialogs
        if (skip) {
            if (Build.VERSION.SDK_INT >= 24 /* Android 7.0 (N) */) {
                skip = false;
            }
        }

        if (skip) {
           Log.v("SDL", "Skip .. Surface is not ready.");
           mIsSurfaceReady = false;
           return;
        }

        /* If the surface has been previously destroyed by onNativeSurfaceDestroyed, recreate it here */
        SDLActivity.onNativeSurfaceChanged();

        /* Surface is ready */
        mIsSurfaceReady = true;

        SDLActivity.mNextNativeState = SDLActivity.NativeState.RESUMED;
        SDLActivity.handleNativeState();
    }

    // Window inset
    @Override
    public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= 30 /* Android 11 (R) */) {
            Insets combined = insets.getInsets(WindowInsets.Type.systemBars() |
                                               WindowInsets.Type.systemGestures() |
                                               WindowInsets.Type.mandatorySystemGestures() |
                                               WindowInsets.Type.tappableElement() |
                                               WindowInsets.Type.displayCutout());

            SDLActivity.onNativeInsetsChanged(combined.left, combined.right, combined.top, combined.bottom);
        }

        // Pass these to any child views in case they need them
        return insets;
    }

    // Key events
    @Override
    public boolean onKey(View v, int keyCode, KeyEvent event) {
        return SDLActivity.handleKeyEvent(v, keyCode, event, null);
    }

    private float getNormalizedX(float x)
    {
        if (mWidth <= 1) {
            return 0.5f;
        } else {
            return (x / (mWidth - 1));
        }
    }

    private float getNormalizedY(float y)
    {
        if (mHeight <= 1) {
            return 0.5f;
        } else {
            return (y / (mHeight - 1));
        }
    }

    // Touch events
    @Override
    public boolean onTouch(View v, MotionEvent event) {
        /* Ref: http://developer.android.com/training/gestures/multi.html */
        int touchDevId = event.getDeviceId();
        final int pointerCount = event.getPointerCount();
        int action = event.getActionMasked();
        int pointerId;
        int i = 0;
        float x,y,p;

        if (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_POINTER_DOWN)
            i = event.getActionIndex();

        do {
            int toolType = event.getToolType(i);

            if (toolType == MotionEvent.TOOL_TYPE_MOUSE) {
                int buttonState = event.getButtonState();
                boolean relative = false;

                // We need to check if we're in relative mouse mode and get the axis offset rather than the x/y values
                // if we are. We'll leverage our existing mouse motion listener
                SDLGenericMotionListener_API14 motionListener = SDLActivity.getMotionListener();
                x = motionListener.getEventX(event, i);
                y = motionListener.getEventY(event, i);
                relative = motionListener.inRelativeMode();

                SDLActivity.onNativeMouse(buttonState, action, x, y, relative);
            } else if (toolType == MotionEvent.TOOL_TYPE_STYLUS || toolType == MotionEvent.TOOL_TYPE_ERASER) {
                pointerId = event.getPointerId(i);
                x = event.getX(i);
                y = event.getY(i);
                p = event.getPressure(i);
                if (p > 1.0f) {
                    // may be larger than 1.0f on some devices
                    // see the documentation of getPressure(i)
                    p = 1.0f;
                }

                // BUTTON_STYLUS_PRIMARY is 2^5, so shift by 4, and apply SDL_PEN_INPUT_DOWN/SDL_PEN_INPUT_ERASER_TIP
                int buttonState = (event.getButtonState() >> 4) | (1 << (toolType == MotionEvent.TOOL_TYPE_STYLUS ? 0 : 30));

                SDLActivity.onNativePen(pointerId, buttonState, action, x, y, p);
            } else { // MotionEvent.TOOL_TYPE_FINGER or MotionEvent.TOOL_TYPE_UNKNOWN
                pointerId = event.getPointerId(i);
                x = getNormalizedX(event.getX(i));
                y = getNormalizedY(event.getY(i));
                p = event.getPressure(i);
                if (p > 1.0f) {
                    // may be larger than 1.0f on some devices
                    // see the documentation of getPressure(i)
                    p = 1.0f;
                }

                SDLActivity.onNativeTouch(touchDevId, pointerId, action, x, y, p);
            }

            // Non-primary up/down
            if (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_POINTER_DOWN)
                break;
        } while (++i < pointerCount);

        return true;
    }

    // Sensor events
    public boolean enableSensor(int sensortype, boolean enabled) {
        if (sensortype == Sensor.TYPE_GYROSCOPE) {
            return enableGyroSensor(enabled);
        }

        // TODO: This uses getDefaultSensor - what if we have >1 accels?
        Sensor sensor = mSensorManager.getDefaultSensor(sensortype);
        if (sensor == null) {
            return false;
        }

        if (enabled) {
            return mSensorManager.registerListener(this,
                            sensor,
                            SensorManager.SENSOR_DELAY_GAME, null);
        } else {
            mSensorManager.unregisterListener(this,
                            sensor);
            return true;
        }
    }

    protected Sensor bestGyroSensor() {
        Sensor sensor = mSensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        if (sensor != null) {
            return sensor;
        }

        if (Build.VERSION.SDK_INT >= 18) {
            sensor = mSensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE_UNCALIBRATED);
            if (sensor != null) {
                return sensor;
            }

            sensor = mSensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
            if (sensor != null) {
                return sensor;
            }
        }

        sensor = mSensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
        if (sensor != null) {
            return sensor;
        }

        sensor = mSensorManager.getDefaultSensor(Sensor.TYPE_GRAVITY);
        if (sensor != null) {
            return sensor;
        }

        return mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
    }

    protected boolean enableGyroSensor(boolean enabled) {
        mGyroscopeEnabled = enabled;
        resetGyroDerivedState();

        if (mGyroscopeSensor != null && !mAccelerometerGyroFallbackEnabled) {
            mSensorManager.unregisterListener(this, mGyroscopeSensor);
        }
        mGyroscopeSensor = null;
        mGyroscopeSensorType = 0;
        mAccelerometerGyroFallbackEnabled = false;

        if (!enabled) {
            return true;
        }

        Sensor sensor = bestGyroSensor();
        if (sensor == null) {
            Log.w("SDL", "No gyro, rotation-vector, gravity, or accelerometer fallback sensor available");
            return false;
        }

        boolean registered = mSensorManager.registerListener(this, sensor, SensorManager.SENSOR_DELAY_GAME, null);
        if (registered) {
            mGyroscopeSensor = sensor;
            mGyroscopeSensorType = sensor.getType();
            mAccelerometerGyroFallbackEnabled = mGyroscopeSensorType == Sensor.TYPE_ACCELEROMETER;
            Log.v("SDL", "Gyro aim sensor enabled: " + sensor.getName() + " type=" + mGyroscopeSensorType);
        } else {
            Log.w("SDL", "Failed to register gyro aim sensor: " + sensor.getName());
        }
        return registered;
    }

    protected void resetGyroDerivedState() {
        mHasLastRotationQuaternion = false;
        mLastRotationVectorTimestamp = 0;
        mHasLastGravityVector = false;
        mLastGravityTimestamp = 0;
        mHasFilteredAccelerometer = false;
        mLastNonAccelerometerGyroTimestamp = 0;
    }

    protected static float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }

    protected void emitGyro(long timestamp, float x, float y, float z, boolean nonAccelerometer) {
        if (nonAccelerometer) {
            mLastNonAccelerometerGyroTimestamp = timestamp;
        }
        SDLActivity.onNativeGyro(x, y, z);
    }

    protected void handleRotationVectorGyro(SensorEvent event) {
        float[] q = new float[4];
        SensorManager.getQuaternionFromVector(q, event.values);

        if (mHasLastRotationQuaternion) {
            float dot = q[0] * mLastRotationQuaternion[0]
                    + q[1] * mLastRotationQuaternion[1]
                    + q[2] * mLastRotationQuaternion[2]
                    + q[3] * mLastRotationQuaternion[3];
            if (dot < 0.0f) {
                q[0] = -q[0];
                q[1] = -q[1];
                q[2] = -q[2];
                q[3] = -q[3];
            }

            float dt = (event.timestamp - mLastRotationVectorTimestamp) * 0.000000001f;
            if (dt > 0.0f && dt < 0.25f) {
                float lw = mLastRotationQuaternion[0];
                float lx = mLastRotationQuaternion[1];
                float ly = mLastRotationQuaternion[2];
                float lz = mLastRotationQuaternion[3];
                float dw = q[0] * lw + q[1] * lx + q[2] * ly + q[3] * lz;
                float dx = -q[0] * lx + q[1] * lw - q[2] * lz + q[3] * ly;
                float dy = -q[0] * ly + q[1] * lz + q[2] * lw - q[3] * lx;
                float dz = -q[0] * lz - q[1] * ly + q[2] * lx + q[3] * lw;

                dw = clamp(dw, -1.0f, 1.0f);
                float angle = 2.0f * (float)Math.acos(dw);
                float sinHalf = (float)Math.sqrt(Math.max(0.0f, 1.0f - dw * dw));
                float scale = sinHalf > 0.0001f ? angle / (sinHalf * dt) : 2.0f / dt;
                emitGyro(event.timestamp, dx * scale, dy * scale, dz * scale, true);
            }
        }

        System.arraycopy(q, 0, mLastRotationQuaternion, 0, 4);
        mLastRotationVectorTimestamp = event.timestamp;
        mHasLastRotationQuaternion = true;
    }

    protected void handleTiltVectorGyroFallback(float x, float y, float z, long timestamp, boolean nonAccelerometer) {
        float mag = (float)Math.sqrt(x * x + y * y + z * z);
        if (mag <= 0.0001f) {
            return;
        }

        x /= mag;
        y /= mag;
        z /= mag;

        if (mHasLastGravityVector) {
            float dt = (timestamp - mLastGravityTimestamp) * 0.000000001f;
            if (dt > 0.0f && dt < 0.25f) {
                float cx = mLastGravityVector[1] * z - mLastGravityVector[2] * y;
                float cy = mLastGravityVector[2] * x - mLastGravityVector[0] * z;
                float cz = mLastGravityVector[0] * y - mLastGravityVector[1] * x;
                float crossMag = (float)Math.sqrt(cx * cx + cy * cy + cz * cz);
                float dot = clamp(mLastGravityVector[0] * x + mLastGravityVector[1] * y + mLastGravityVector[2] * z, -1.0f, 1.0f);
                if (crossMag > 0.0001f) {
                    float angle = (float)Math.atan2(crossMag, dot);
                    float scale = angle / (crossMag * dt);
                    emitGyro(timestamp, cx * scale, cy * scale, cz * scale, nonAccelerometer);
                }
            }
        }

        mLastGravityVector[0] = x;
        mLastGravityVector[1] = y;
        mLastGravityVector[2] = z;
        mLastGravityTimestamp = timestamp;
        mHasLastGravityVector = true;
    }

    protected void handleGravityGyroFallback(SensorEvent event) {
        handleTiltVectorGyroFallback(event.values[0], event.values[1], event.values[2], event.timestamp, true);
    }

    protected void handleAccelerometerGyroFallback(SensorEvent event) {
        if (!mHasFilteredAccelerometer) {
            mFilteredAccelerometer[0] = event.values[0];
            mFilteredAccelerometer[1] = event.values[1];
            mFilteredAccelerometer[2] = event.values[2];
            mHasFilteredAccelerometer = true;
        } else {
            // Estimate gravity from raw accelerometer data.  This keeps the
            // fallback usable on devices without gyro / rotation-vector sensors
            // while damping quick linear motion from hand movement.
            float alpha = 0.86f;
            mFilteredAccelerometer[0] = mFilteredAccelerometer[0] * alpha + event.values[0] * (1.0f - alpha);
            mFilteredAccelerometer[1] = mFilteredAccelerometer[1] * alpha + event.values[1] * (1.0f - alpha);
            mFilteredAccelerometer[2] = mFilteredAccelerometer[2] * alpha + event.values[2] * (1.0f - alpha);
        }

        handleTiltVectorGyroFallback(
                mFilteredAccelerometer[0],
                mFilteredAccelerometer[1],
                mFilteredAccelerometer[2],
                event.timestamp,
                false);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // TODO
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {

            // Since we may have an orientation set, we won't receive onConfigurationChanged events.
            // We thus should check here.
            int newRotation;

            float x, y;
            switch (mDisplay.getRotation()) {
                case Surface.ROTATION_0:
                default:
                    x = event.values[0];
                    y = event.values[1];
                    newRotation = 0;
                    break;
                case Surface.ROTATION_90:
                    x = -event.values[1];
                    y = event.values[0];
                    newRotation = 90;
                    break;
                case Surface.ROTATION_180:
                    x = -event.values[0];
                    y = -event.values[1];
                    newRotation = 180;
                    break;
                case Surface.ROTATION_270:
                    x = event.values[1];
                    y = -event.values[0];
                    newRotation = 270;
                    break;
            }

            if (newRotation != SDLActivity.mCurrentRotation) {
                SDLActivity.mCurrentRotation = newRotation;
                SDLActivity.onNativeRotationChanged(newRotation);
            }

            SDLActivity.onNativeAccel(-x / SensorManager.GRAVITY_EARTH,
                                      y / SensorManager.GRAVITY_EARTH,
                                      event.values[2] / SensorManager.GRAVITY_EARTH);

            if (mGyroscopeEnabled
                    && (mAccelerometerGyroFallbackEnabled
                    || mLastNonAccelerometerGyroTimestamp == 0
                    || event.timestamp - mLastNonAccelerometerGyroTimestamp > 250000000L)) {
                handleAccelerometerGyroFallback(event);
            }

        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            emitGyro(event.timestamp, event.values[0], event.values[1], event.values[2], true);
        } else if (Build.VERSION.SDK_INT >= 18 && event.sensor.getType() == Sensor.TYPE_GYROSCOPE_UNCALIBRATED) {
            emitGyro(event.timestamp, event.values[0], event.values[1], event.values[2], true);
        } else if ((Build.VERSION.SDK_INT >= 18 && event.sensor.getType() == Sensor.TYPE_GAME_ROTATION_VECTOR)
                || event.sensor.getType() == Sensor.TYPE_ROTATION_VECTOR) {
            handleRotationVectorGyro(event);
        } else if (event.sensor.getType() == Sensor.TYPE_GRAVITY && mGyroscopeEnabled) {
            handleGravityGyroFallback(event);
        }
    }

    // Prevent android internal NullPointerException (https://github.com/libsdl-org/SDL/issues/13306)
    @Override
    public PointerIcon onResolvePointerIcon(MotionEvent event, int pointerIndex) {
        try {
            return super.onResolvePointerIcon(event, pointerIndex);
        } catch (NullPointerException e) {
            return null;
        }
    }

    // Captured pointer events for API 26.
    public boolean onCapturedPointerEvent(MotionEvent event)
    {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();

        for (int i = 0; i < pointerCount; i++) {
            float x, y;
            switch (action) {
                case MotionEvent.ACTION_SCROLL:
                    x = event.getAxisValue(MotionEvent.AXIS_HSCROLL, i);
                    y = event.getAxisValue(MotionEvent.AXIS_VSCROLL, i);
                    SDLActivity.onNativeMouse(0, action, x, y, false);
                    return true;

                case MotionEvent.ACTION_HOVER_MOVE:
                case MotionEvent.ACTION_MOVE:
                    x = event.getX(i);
                    y = event.getY(i);
                    SDLActivity.onNativeMouse(0, action, x, y, true);
                    return true;

                case MotionEvent.ACTION_BUTTON_PRESS:
                case MotionEvent.ACTION_BUTTON_RELEASE:

                    // Change our action value to what SDL's code expects.
                    if (action == MotionEvent.ACTION_BUTTON_PRESS) {
                        action = MotionEvent.ACTION_DOWN;
                    } else { /* MotionEvent.ACTION_BUTTON_RELEASE */
                        action = MotionEvent.ACTION_UP;
                    }

                    x = event.getX(i);
                    y = event.getY(i);
                    int button = event.getButtonState();

                    SDLActivity.onNativeMouse(button, action, x, y, true);
                    return true;
            }
        }      

        return false;
    }
}
