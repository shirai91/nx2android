package org.nx2a;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.Reader;
import java.util.ArrayList;

/** The same atlas manifest used by the native on-screen gamepad. */
final class OverlayTheme {
    static final class Sprite {
        String control, direction, state;
        float u, v, width, height, x, y;
    }
    final ArrayList<Sprite> sprites = new ArrayList<>();
    String image;
    float designHeight, scale, travel, alpha = 1;

    static OverlayTheme read(Reader source, String layout) throws IOException {
        OverlayTheme theme = new OverlayTheme();
        BufferedReader in = new BufferedReader(source);
        String line;
        try {
            while ((line = in.readLine()) != null) {
                String[] p = line.split("\\t");
                if (p.length == 2 && p[0].equals("alpha")) theme.alpha = Float.parseFloat(p[1]);
                else if (p.length == 7 && p[0].equals("layout") && p[1].equals(layout)) {
                    theme.designHeight = Float.parseFloat(p[3]); theme.image = p[4];
                    theme.scale = Float.parseFloat(p[5]); theme.travel = Float.parseFloat(p[6]);
                } else if (p.length == 11 && p[0].equals("sprite") && p[1].equals(layout)) {
                    Sprite s = new Sprite(); s.control = p[2]; s.direction = p[3]; s.state = p[4];
                    s.u = Float.parseFloat(p[5]); s.v = Float.parseFloat(p[6]); s.width = Float.parseFloat(p[7]);
                    s.height = Float.parseFloat(p[8]); s.x = Float.parseFloat(p[9]); s.y = Float.parseFloat(p[10]);
                    theme.sprites.add(s);
                }
            }
        } catch (NumberFormatException e) { throw new IOException("Invalid overlay theme", e); }
        if (theme.image == null || theme.designHeight <= 0 || theme.scale <= 0 || theme.sprites.isEmpty())
            throw new IOException("Missing overlay theme layout: " + layout);
        return theme;
    }
}
