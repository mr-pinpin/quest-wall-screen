package com.pinpin.wallscreen.probe;
import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import org.json.JSONObject;
public final class MainActivity extends Activity {
    static { System.loadLibrary("pinpin_wall_probe"); }
    private static native String inspect(Context context);
    public void onCreate(Bundle state) {
        super.onCreate(state);
        final TextView text=new TextView(this);
        text.setTextSize(24);text.setPadding(32,32,32,32);text.setText("Checking native room capabilities…");setContentView(text);
        new Thread(new Runnable(){public void run(){
            String result;
            try {result=inspect(getApplicationContext());}
            catch(Throwable e){result="{\"error\":"+JSONObject.quote(e.toString())+"}";}
            try {FileOutputStream f=new FileOutputStream(new File(getFilesDir(),"runtime.json"));f.write(result.getBytes("UTF-8"));f.close();}catch(Exception ignored){}
            final String report=result;
            runOnUiThread(new Runnable(){public void run(){text.setText(report);new Handler().postDelayed(new Runnable(){public void run(){finish();}},2000);}});
        }},"native-room-probe").start();
    }
}
