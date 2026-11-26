// Four visible players, each with up to two independent input sources. Zero means empty.
#pragma once
#include <stdint.h>

enum { PA_PLAYERS = 4, PA_CELLS = 2, PA_SOURCES = 17 };
typedef struct { int source[PA_PLAYERS][PA_CELLS]; } pad_assignment;
typedef struct { uint64_t buttons, moved[2]; int32_t axis[4]; } pad_assignment_input;

static inline void pa_axes(pad_assignment_input *s, const int32_t axis[4], uint64_t *clock) {
    for (int stick = 0; stick < 2; stick++) {
        int n = stick * 2;
        if ((axis[n] != s->axis[n] || axis[n + 1] != s->axis[n + 1]) && (axis[n] || axis[n + 1]))
            s->moved[stick] = ++*clock;
        s->axis[n] = axis[n]; s->axis[n + 1] = axis[n + 1];
    }
}

static inline int pa_find(const pad_assignment *a, int source, int *player, int *cell) {
    if (source <= 0) return 0;
    for (int p = 0; p < PA_PLAYERS; p++) for (int c = 0; c < PA_CELLS; c++)
        if (a->source[p][c] == source) { *player = p; *cell = c; return 1; }
    return 0;
}

// cell -1 adds to the first free place; player -1 returns the source to the pool.
// Dropping on a card exchanges positions; from the pool the displaced card returns there.
static inline int pa_move(pad_assignment *a, int source, int player, int cell) {
    if (source <= 0 || player < -1 || player >= PA_PLAYERS || cell < -1 || cell >= PA_CELLS) return 0;
    int oldp = -1, oldc = -1; int assigned = pa_find(a, source, &oldp, &oldc);
    if (player < 0) { if (assigned) a->source[oldp][oldc] = 0; return 1; }
    if (cell < 0) {
        for (cell = 0; cell < PA_CELLS && a->source[player][cell]; cell++);
        if (cell == PA_CELLS) return 0;
    }
    int displaced = a->source[player][cell];
    if (displaced == source) return 1;
    if (assigned) a->source[oldp][oldc] = displaced;
    a->source[player][cell] = source;
    return 1;
}

// The most recently moved non-neutral stick owns each axis pair, separately from buttons.
static inline pad_assignment_input pa_combine(const pad_assignment_input *a, const pad_assignment_input *b) {
    pad_assignment_input out = {0}; out.buttons = a->buttons | b->buttons;
    for (int stick = 0; stick < 2; stick++) {
        int n = stick * 2;
        int an = a->axis[n] || a->axis[n + 1], bn = b->axis[n] || b->axis[n + 1];
        const pad_assignment_input *s = bn && (!an || b->moved[stick] > a->moved[stick]) ? b : a;
        out.axis[n] = s->axis[n]; out.axis[n + 1] = s->axis[n + 1]; out.moved[stick] = s->moved[stick];
    }
    return out;
}

typedef struct {
    pad_assignment slots;
    pad_assignment_input input[PA_SOURCES];
    int connected[PA_SOURCES], last_player[PA_SOURCES], last_cell[PA_SOURCES];
} pad_assignment_runtime;

static inline void pa_runtime_init(pad_assignment_runtime *r) {
    *r = (pad_assignment_runtime){0};
    for (int i = 0; i < PA_SOURCES; i++) r->last_player[i] = r->last_cell[i] = -1;
    r->connected[0] = 1; /* the phone is always in the pool */
}
static inline int pa_runtime_move(pad_assignment_runtime *r, int source, int player, int cell) {
    if (source < 1 || source > PA_SOURCES || !r->connected[source - 1]) return 0;
    if (!pa_move(&r->slots, source, player, cell)) return 0;
    for (int i = 0; i < PA_SOURCES; i++) if (r->connected[i]) {
        int p = -1, c = -1; pa_find(&r->slots, i + 1, &p, &c);
        r->last_player[i] = p; r->last_cell[i] = c;
    }
    return 1;
}
static inline void pa_runtime_detach(pad_assignment_runtime *r, int source) {
    if (source <= 1 || source > PA_SOURCES) return;
    int i = source - 1, p = -1, c = -1;
    if (pa_find(&r->slots, source, &p, &c)) { r->last_player[i] = p; r->last_cell[i] = c; pa_move(&r->slots, source, -1, -1); }
    r->input[i] = (pad_assignment_input){0}; r->connected[i] = 0;
}
static inline void pa_runtime_attach(pad_assignment_runtime *r, int source) {
    if (source <= 1 || source > PA_SOURCES) return;
    int i = source - 1, p = r->last_player[i], c = r->last_cell[i]; r->connected[i] = 1;
    if (p >= 0 && c >= 0 && !r->slots.source[p][c]) pa_move(&r->slots, source, p, c);
}

// A newly discovered controller takes an entirely empty player; reconnects retain their prior choice.
static inline void pa_runtime_attach_new(pad_assignment_runtime *r, int source, int known) {
    pa_runtime_attach(r, source);
    if (known) return;
    for (int p = 0; p < PA_PLAYERS; p++) if (!r->slots.source[p][0] && !r->slots.source[p][1]) {
        pa_runtime_move(r, source, p, -1); break;
    }
}
