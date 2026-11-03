#pragma once
#include <android/input.h>

// A move can replace only a move from the same input stream, never a key or another device.
static inline int external_moves_match(const AInputEvent *old, const AInputEvent *next) {
    return AInputEvent_getType(old) == AINPUT_EVENT_TYPE_MOTION && AInputEvent_getType(next) == AINPUT_EVENT_TYPE_MOTION &&
        AInputEvent_getSource(old) == AInputEvent_getSource(next) && AInputEvent_getDeviceId(old) == AInputEvent_getDeviceId(next) &&
        (AMotionEvent_getAction(old) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_MOVE &&
        (AMotionEvent_getAction(next) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_MOVE;
}
