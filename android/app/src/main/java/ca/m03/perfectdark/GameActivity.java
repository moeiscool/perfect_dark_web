package ca.m03.perfectdark;

import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The game: SDL runs the native host (libmain.so), which runs the web build's pd.wasm.
 * Paths: the ROM in files/data, saves and settings in files/save (Android/data/ca.m03.perfectdark).
 */
public class GameActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File base = getExternalFilesDir(null);
        File data = new File(base, "data");
        File save = new File(base, "save");
        //noinspection ResultOfMethodCallIgnored
        save.mkdirs();
        List<String> args = new ArrayList<>(Arrays.asList(
                "--data", data.getAbsolutePath(),
                "--save", save.getAbsolutePath(),
                "--tmp", new File(getCacheDir(), "pd").getAbsolutePath(),
                "--fullscreen"));
        String extra = getIntent().getStringExtra("pdargs");
        if (extra != null && !extra.trim().isEmpty()) {
            args.addAll(Arrays.asList(extra.trim().split("\\s+")));
        }
        return args.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        hideSystemUi();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUi();
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
}
