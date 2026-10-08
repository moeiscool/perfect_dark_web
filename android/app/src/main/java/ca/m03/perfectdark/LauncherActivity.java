package ca.m03.perfectdark;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;

/**
 * Makes sure the ROM is in the app's folder (Android/data/ca.m03.perfectdark/files/data), then
 * starts the game. The ROM is picked once with the system file picker and checked by its MD5.
 */
public class LauncherActivity extends Activity {
    static final String ROM_NAME = "pd.ntsc-final.z64";
    private static final String MD5_V11 = "e03b088b6ac9e0080440efed07c1e40f"; // NTSC v1.1, recommended
    private static final String MD5_V10 = "7f4171b0c8d17815be37913f535e4e93"; // NTSC v1.0, works
    private static final String ACTION_CHANGE_ROM = "ca.m03.perfectdark.CHANGE_ROM";
    private static final int PICK_ROM = 1;

    private TextView status;
    private Button pick;

    static File romFile(Activity a) {
        return new File(new File(a.getExternalFilesDir(null), "data"), ROM_NAME);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        buildUi();

        final boolean change = ACTION_CHANGE_ROM.equals(getIntent().getAction());
        final File rom = romFile(this);
        if (!change && rom.length() == 32 * 1024 * 1024) {
            startGame();
            return;
        }
        status.setText(change
                ? "Choose a different Perfect Dark ROM."
                : "Choose your Perfect Dark ROM (NTSC v1.1, .z64).\n\nIt is copied into the app's own folder; nothing is uploaded.");
    }

    private void buildUi() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(64, 64, 64, 64);
        layout.setBackgroundColor(Color.rgb(11, 13, 17));

        TextView title = new TextView(this);
        title.setText("Perfect Dark");
        title.setTextColor(Color.rgb(216, 64, 47));
        title.setTextSize(32);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(230, 233, 238));
        status.setTextSize(16);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, 32, 0, 32);
        layout.addView(status);

        pick = new Button(this);
        pick.setText("Choose ROM…");
        pick.setOnClickListener(v -> openPicker());
        layout.addView(pick);
        setContentView(layout);
        pick.requestFocus(); // controllers and TV remotes
    }

    private void openPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ROM || result != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        status.setText("Copying the ROM…");
        pick.setEnabled(false);
        new Thread(() -> {
            String error = null;
            String md5 = null;
            try {
                md5 = copyRom(uri);
            } catch (IOException e) {
                error = e.getMessage();
            }
            final String err = error, sum = md5;
            runOnUiThread(() -> onCopied(err, sum));
        }).start();
    }

    // copies the picked file over the app's ROM; returns its MD5
    private String copyRom(Uri uri) throws IOException {
        File rom = romFile(this);
        //noinspection ResultOfMethodCallIgnored
        rom.getParentFile().mkdirs();
        File tmp = new File(rom.getParentFile(), ROM_NAME + ".part");
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) {
                throw new IOException("can't open the file");
            }
            MessageDigest md = MessageDigest.getInstance("MD5");
            byte[] buf = new byte[1 << 16];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                md.update(buf, 0, n);
                total += n;
                if (total > 64L * 1024 * 1024) {
                    throw new IOException("that file is too big to be a Perfect Dark ROM");
                }
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) {
                hex.append(String.format("%02x", b));
            }
            if (!tmp.renameTo(rom)) {
                throw new IOException("can't save the ROM");
            }
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e.getMessage());
        } finally {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
        }
    }

    private void onCopied(String error, String md5) {
        pick.setEnabled(true);
        if (error != null) {
            status.setText("Couldn't use that file: " + error);
            return;
        }
        if (MD5_V11.equals(md5)) {
            startGame();
        } else if (MD5_V10.equals(md5)) {
            new AlertDialog.Builder(this)
                    .setTitle("NTSC v1.0 ROM")
                    .setMessage("This is the v1.0 ROM. It works, but v1.1 is recommended, and online matches need v1.1.")
                    .setPositiveButton("Play anyway", (d, w) -> startGame())
                    .setNegativeButton("Choose another", (d, w) -> openPicker())
                    .show();
        } else {
            //noinspection ResultOfMethodCallIgnored
            romFile(this).delete();
            status.setText("That isn't a supported Perfect Dark ROM (it needs the NTSC v1.1 .z64 dump). Choose another file.");
        }
    }

    private void startGame() {
        Intent game = new Intent(this, GameActivity.class);
        // test runs pass extra game arguments through ("pdargs")
        if (getIntent().hasExtra("pdargs")) {
            game.putExtra("pdargs", getIntent().getStringExtra("pdargs"));
        }
        startActivity(game);
        finish();
    }
}
