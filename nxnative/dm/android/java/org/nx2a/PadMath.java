package org.nx2a;

final class PadMath {
    private static final long RIGHT=1L<<14, DOWN=1L<<15, LEFT=1L<<12, UP=1L<<13;
    private static final long[] DIRECTIONS={RIGHT, RIGHT|DOWN, DOWN, DOWN|LEFT, LEFT, LEFT|UP, UP, UP|RIGHT};
    static long direction(float x, float y) {
        if (Math.hypot(x, y) <= .25) return 0;
        return DIRECTIONS[((int)Math.round(Math.atan2(y, x)/(Math.PI/4)) + 8) % 8];
    }
}
