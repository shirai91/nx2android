// Switch 2 SDK calls through services the host does not serve (found on Donkey Kong Bananza): answered as a console without the hardware would.
#include "hle.h"
#include <stdatomic.h>

// ---------- hid: the Joy-Con 2 mouse sensor (JCMS). InitializeJcms opens the hid service itself. A handle is the npad id | JcmsType << 8, as the SDK makes it;
// no controller here has the sensor, so there are no states ----------
HLE_FORCE("_ZN2nn3hid14InitializeJcmsEPvm", "Joy-Con mouse sensor: none on the host", void, h_jcms_Init, void *work, uint64_t size) { (void)work; (void)size; }
HLE_FORCE("_ZN2nn3hid12FinalizeJcmsEv", "Joy-Con mouse sensor: nothing to release", void, h_jcms_Fini, void) {}
HLE_FORCE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", "Joy-Con mouse sensor handle: npad id | type << 8", uint32_t, h_jcms_Handle, const uint32_t *id, const uint32_t *type) {
    return (*id & 0xff) | (*type & 0xff) << 8; }
HLE_FORCE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", "Joy-Con mouse sensor: none on the host", void, h_jcms_Start, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", "Joy-Con mouse sensor: none on the host", void, h_jcms_Stop, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", "Joy-Con mouse sensor states: none", int, h_jcms_States, void *out, int n, const uint32_t *h) {
    (void)out; (void)n; (void)h; return 0; }

// ---------- oe: the graphics scale, which the SDK reads from a session of am's the host never opened (a null read in oe::GetGraphicsScalingParameters). The game keeps it as a
// draw-size tier (Donkey Kong Bananza main+0x549348); answered by the operation mode: 0 handheld, 1 console. It changes only with the mode ----------
HLE_FORCE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", "graphics scale: 0 handheld, 1 console", int, h_oe_GfxScale, void) { return dm_opmode() == 1; }
HLE_FORCE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", "graphics scale change notification: the scale follows the operation mode", void, h_oe_GfxScaleNotify, unsigned on) { (void)on; }

// ---------- audio: output volume auto-tune, read at boot through the audio device service (aud:u). Kept for the getter, off at start, not applied ----------
static atomic_uchar g_autotune;
HLE_FORCE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", "output volume auto-tune: kept for the getter", void, h_aud_SetAutoTune, unsigned on) { atomic_store(&g_autotune, on & 1); }
HLE_FORCE("_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", "output volume auto-tune: what the game set (off at start)", unsigned, h_aud_AutoTune, void) { return atomic_load(&g_autotune); }

// ---------- streamplay: GameShare / GameChat streaming (stpl:u), initialized at boot. The host streams to nobody: the library is ready, no session opens and no guest
// connects, so every guest pad, sensor, vibration and mouse query has nothing to report. Opening a session is a player's action and stays unhooked ----------
static uint8_t g_stpl_event[0x80] __attribute__((aligned(16)));
static _Atomic uint32_t g_stpl_styles, g_stpl_hold;
HLE_FORCE("_ZN2nn10streamplay21GetRequiredMemorySizeENS0_14StreamPlayTypeENS0_13StreamingModeEi", "streamplay work memory: a page, unused", uint32_t, h_stpl_MemSize, int type, int mode, int n) {
    (void)type; (void)mode; (void)n; return 0x1000; }
HLE_FORCE("_ZN2nn10streamplay10InitializeENS0_14StreamPlayTypeENS0_13StreamingModeEPvm", "streamplay: ready, no session", Result, h_stpl_Init, int type, int mode, void *work, uint64_t size) {
    (void)type; (void)mode; (void)work; (void)size; return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay8FinalizeEv", "streamplay: nothing to release", void, h_stpl_Fini, void) {}
HLE_FORCE("_ZN2nn10streamplay30GetConnectionStateChangedEventEv", "streamplay connection event: bound, never signalled", void *, h_stpl_Event, void) {
    hle_bind_system_event(g_stpl_event, 0); return g_stpl_event; }
HLE_FORCE("_ZN2nn10streamplay18GetConnectionStateEPNS0_15ConnectionStateE", "streamplay connection state: not connected", void, h_stpl_State, uint32_t *s) { *s = 0; }
HLE_FORCE("_ZN2nn10streamplay29SetSupportedGuestNpadStyleSetENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", "guest pad styles: kept", void, h_stpl_SetStyles, uint32_t s) { g_stpl_styles = s; }
HLE_FORCE("_ZN2nn10streamplay23SetGuestNpadJoyHoldTypeENS_3hid15NpadJoyHoldTypeE", "guest Joy-Con hold type: kept", void, h_stpl_SetHold, uint32_t t) { g_stpl_hold = t; }
HLE_FORCE("_ZN2nn10streamplay23GetGuestNpadJoyHoldTypeEv", "guest Joy-Con hold type: what the game set", uint32_t, h_stpl_GetHold, void) { return g_stpl_hold; }
HLE_FORCE("_ZN2nn10streamplay25SetSupportedHidFeatureSetENS_4util10BitFlagSetILi32ENS0_13HidFeatureTagEEE", "guest hid features: no guest", void, h_stpl_HidFeat, uint32_t s) { (void)s; }
HLE_FORCE("_ZN2nn10streamplay19GetGuestPadStyleSetERKNS0_10GuestPadIdE", "guest pad style: none", uint32_t, h_stpl_PadStyle, const void *id) { (void)id; return 0; }
// The queries that fill an array return how many entries they filled: none
#define STPL_NONE(mangled, cname) HLE_FORCE(mangled, "streamplay guest query: no guest", int, cname, void *out, int n, const void *id) { (void)out; (void)n; (void)id; return 0; }
STPL_NONE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadFullKeyStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadFull)
STPL_NONE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadHandheldStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadHandheld)
STPL_NONE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyDualStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadDual)
STPL_NONE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadJoyRightStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadRight)
STPL_NONE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyLeftStateEEEiRKNS0_10GuestPadIdE", h_stpl_PadLeft)
STPL_NONE("_ZN2nn10streamplay27GetGuestSixAxisSensorStatesEPNS0_23GuestSixAxisSensorStateEiRKNS0_24GuestSixAxisSensorHandleE", h_stpl_SixStates)
STPL_NONE("_ZN2nn10streamplay18GetGuestJcmsStatesEPNS0_14GuestJcmsStateEiRKNS0_15GuestJcmsHandleE", h_stpl_JcmsStates)
#define STPL_NONE_STYLE(mangled, cname) HLE_FORCE(mangled, "streamplay guest query: no guest", int, cname, void *out, int n, const void *id, uint32_t style) { \
    (void)out; (void)n; (void)id; (void)style; return 0; }
STPL_NONE_STYLE("_ZN2nn10streamplay28GetGuestSixAxisSensorHandlesEPNS0_24GuestSixAxisSensorHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", h_stpl_SixHandles)
STPL_NONE_STYLE("_ZN2nn10streamplay30GetGuestVibrationDeviceHandlesEPNS0_26GuestVibrationDeviceHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", h_stpl_VibHandles)
HLE_FORCE("_ZN2nn10streamplay27GetGuestVibrationDeviceInfoEPNS_3hid19VibrationDeviceInfoERKNS0_26GuestVibrationDeviceHandleE", "guest vibration device: none", void, h_stpl_VibInfo,
          uint64_t *info, const void *h) { (void)h; *info = 0; }
HLE_FORCE("_ZN2nn10streamplay18GetGuestJcmsHandleERKNS0_10GuestPadIdERKNS_3hid8JcmsTypeE", "guest Joy-Con mouse: none", uint32_t, h_stpl_JcmsHandle, const void *id, const void *type) {
    (void)id; (void)type; return 0; }

// ---------- vi: HDR, VRR and extended swap intervals. The SDK forwards the layer to vi2, whose layer object the host never made (a call through null from
// SetLayerNextHdrContentGamutMetadata, main+0x2036f4). The host presents SDR at a fixed rate: no HDR, no VRR, layer metadata has nothing to apply to, and NVN's swap interval
// stays in effect. Paper white is BT.2408's 203 nits, read only with HDR on ----------
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR: off (SDR output)", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) { (void)disp; *on = 0; *fmt = 0; *peak = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display paper white: 203 nits", Result, h_vi_PaperWhite, uint16_t *nits, const void *disp) {
    (void)disp; *nits = 203; return RES_OK; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "display VRR: not supported", Result, h_vi_Vrr, uint8_t *on, const void *disp) { (void)disp; *on = 0; return RES_OK; }
#define VI_NOTHING(mangled, cname, ...) HLE_FORCE(mangled, "vi: nothing to apply on an SDR, fixed-rate output", Result, cname, __VA_ARGS__) { return RES_OK; }
VI_NOTHING("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", h_vi_SetVrr, void *disp, unsigned on)
VI_NOTHING("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", h_vi_ExtSwap, void *layer, int a, int b)
VI_NOTHING("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", h_vi_ExtSwapClear, void *layer)
VI_NOTHING("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", h_vi_HdrGamut, void *layer, int g)
VI_NOTHING("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", h_vi_HdrToneMap, void *layer, int m)
VI_NOTHING("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", h_vi_HdrExtLum, void *layer, uint32_t lo, uint32_t hi)
VI_NOTHING("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", h_vi_HdrStdLum, void *layer, uint32_t lo, uint32_t hi)
