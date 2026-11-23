#include "hle_audio.c"
#include <assert.h>
#include <time.h>

// Supply a host key and an auto-clear guest-event substitute. The queue,
// stop hooks, silent sink and host notification thread are all production C.
static const void *mapped_key;
static void *mapped_object;

void *objmap_get(const void *key) { return key == mapped_key ? mapped_object : NULL; }
void objmap_put(const void *key, void *object) { assert(!mapped_object); mapped_key = key; mapped_object = object; }
void *objmap_del(const void *key) {
    assert(key == mapped_key);
    void *object = mapped_object;
    mapped_key = NULL;
    mapped_object = NULL;
    return object;
}
module_t *module_of(const void *address) { (void)address; return NULL; }
void dm_guest_write_prepare(void *address, size_t size) { (void)address; (void)size; }
void dm_host_helper_qos(qos_class_t qos) { (void)qos; }
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
// The isolated host has no main.c logging clock.
double dm_log_elapsed(void) { return 0; }
void hle_bind_system_event(void *event, int autoclear) { assert(event && autoclear); }
void hle_signal_system_event(void *event) { dispatch_semaphore_signal(event); }

static int wait_release(dispatch_semaphore_t event, int ms) {
    return dispatch_semaphore_wait(event, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000)) == 0;
}

static void stop_returns_pending_and_partial_buffers(void *key) {
    aout *a = ao(key);
    int16_t samples[] = {11, 22, 33, 44};
    gbuf first = {samples, sizeof samples, sizeof samples};
    gbuf partial = {samples, sizeof samples, sizeof samples};
    gbuf last = {samples, sizeof samples, sizeof samples};
    assert(h_au_Append(key, &first) && h_au_Append(key, &partial) && h_au_Append(key, &last));

    // Manually drive the consumer without a device or a timed sink. Finish the
    // first buffer and consume one sample of the second before stopping.
    atomic_store(&a->state, STATE_STARTED);
    int16_t played[5];
    pull(a, (uint8_t *)played, sizeof played);
    assert(h_au_BufCount(key) == 2);
    h_au_Stop(key);
    assert(h_au_BufCount(key) == 0);
    assert(h_au_Released(key) == &first);
    assert(h_au_Released(key) == &partial);
    assert(h_au_Released(key) == &last);
    assert(h_au_Released(key) == NULL);

    // Reusing a buffer after a restart starts at the first sample.
    assert(h_au_Append(key, &partial));
    atomic_store(&a->state, STATE_STARTED);
    pull(a, (uint8_t *)played, sizeof played[0]);
    assert(played[0] == samples[0]);
    h_au_Stop(key);
    assert(h_au_Released(key) == &partial);
}

static void stop_wakes_guest(void *key, dispatch_semaphore_t event, int has_audio) {
    // A 1 GiB block of implicit silence cannot naturally finish during this test.
    gbuf queued = {NULL, 1u << 30, 1u << 30};
    if (has_audio) assert(h_au_Append(key, &queued));
    assert(h_au_Start(key) == RES_OK);
    h_au_Stop(key);
    assert(wait_release(event, 1000));
    assert(h_au_Released(key) == (has_audio ? &queued : NULL));
    assert(h_au_Append(key, &queued));
    h_au_Stop(key);
    assert(!wait_release(event, 50));
    assert(h_au_BufCount(key) == 1);
    assert(h_au_Released(key) == NULL);
    assert(h_au_Start(key) == RES_OK);
    h_au_Stop(key);
    assert(h_au_Released(key) == &queued);
}

static void capacity_counts_releases_until_reclaimed(void *key) {
    gbuf queued[QMAX + 1] = {0};
    aout *a = ao(key);
    atomic_store(&a->state, STATE_STARTED);
    int16_t sample;
    for (size_t i = 0; i < QMAX - 1; i++) {
        queued[i].size = sizeof sample;
        assert(h_au_Append(key, &queued[i]));
        pull(a, (uint8_t *)&sample, sizeof sample);
    }
    assert(h_au_Append(key, &queued[QMAX - 1]));
    assert(!h_au_Append(key, &queued[QMAX]));
    assert(h_au_Released(key) == &queued[0]);
    assert(h_au_Append(key, &queued[QMAX]));
    h_au_Stop(key);
    for (size_t i = 1; i <= QMAX; i++) assert(h_au_Released(key) == &queued[i]);
    assert(h_au_Released(key) == NULL);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(setenv("DM_AUDIO", "0", 1) == 0);
    uint32_t key = 0;
    dispatch_semaphore_t event = dispatch_semaphore_create(0);
    _Alignas(uint32_t) const uint8_t param[8] = {0};
    assert(h_au_OpenDef(&key, event, param) == RES_OK);

    if (!strcmp(argv[1], "fifo")) stop_returns_pending_and_partial_buffers(&key);
    else if (!strcmp(argv[1], "event")) stop_wakes_guest(&key, event, 1);
    else if (!strcmp(argv[1], "empty")) stop_wakes_guest(&key, event, 0);
    else if (!strcmp(argv[1], "capacity")) capacity_counts_releases_until_reclaimed(&key);
    else if (!strcmp(argv[1], "backend")) {
        // Let the real silent sink observe backend state during repeated lifecycle changes.
        for (int i = 0; i < 20; i++) {
            assert(h_au_Start(&key) == RES_OK);
            usleep(6000);
            h_au_Stop(&key);
        }
    }
    else assert(0);

    h_au_Close(&key);
    dispatch_release(event);
    return 0;
}
