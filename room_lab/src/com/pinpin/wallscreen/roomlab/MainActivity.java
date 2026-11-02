package com.pinpin.wallscreen.roomlab;
import android.app.Activity;
import android.os.Bundle;

// Thin host: the OpenXR session, rendering and room logic live in room_lab.cpp.
public final class MainActivity extends Activity {
    static { System.loadLibrary("openxr_loader"); System.loadLibrary("pinpin_room_lab"); }
    private static native void nativeStart(Activity activity, String filesDir, boolean requestCapture);
    private static native void nativeStop();
    public void onCreate(Bundle state) {
        super.onCreate(state);
        if (checkSelfPermission("com.oculus.permission.USE_SCENE") != 0)
            requestPermissions(new String[]{"com.oculus.permission.USE_SCENE"}, 1);
        nativeStart(this, getFilesDir().getAbsolutePath(), getIntent().getBooleanExtra("capture", false));
    }
    public void onDestroy() { nativeStop(); super.onDestroy(); }
}
