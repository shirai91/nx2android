package org.nx2a;

import android.app.NativeActivity;
import android.content.Intent;

public class ImportActivity extends NativeActivity {
    public void importModZip() { ModImport.open(this); }
    public void manageMods() { ModManager.open(this); }
    public String startMods(boolean conflict) { return ModManager.start(this, conflict); }
    @Override protected void onResume() { super.onResume(); ModImport.attach(this); }
    @Override protected void onPause() { ModImport.detach(this); super.onPause(); }
    @Override protected void onDestroy() { ModImport.detach(this); super.onDestroy(); }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        ModImport.result(this, request, result, data);
    }
}
