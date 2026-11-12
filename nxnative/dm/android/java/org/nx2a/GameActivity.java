package org.nx2a;

import android.app.NativeActivity;
import android.content.Intent;

/** The game's activity when porter could build the app's Java (pipeline.py java_bridge): NativeActivity with the save manager's pickers (SaveBridge).
 *  --native-keyboard adds the text entry (KeyboardGameActivity); without any Java the APK stays code-less and the menu keeps its own save manager. */
public class GameActivity extends NativeActivity {
    private final SaveBridge saves = new SaveBridge(this);

    public SaveBridge saveBridge() { return saves; }   // saves_android.c looks it up: its absence means a code-less app

    @Override protected void onActivityResult(int request, int result, Intent data) {
        if (!saves.onActivityResult(request, result, data)) super.onActivityResult(request, result, data);
    }
}
