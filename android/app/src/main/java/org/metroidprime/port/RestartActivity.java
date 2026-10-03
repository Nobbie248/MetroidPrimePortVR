package org.metroidprime.port;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

// Ends the game's process and starts it again, so a data folder move takes
// effect: the folder is chosen once at startup. Runs in its own process
// (AndroidManifest.xml), started while the game is in front, so the system
// lets it open the game again.
public final class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) {
            Process.killProcess(pid);
        }
        Intent intent = new Intent(this, MetroidPrimeActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
