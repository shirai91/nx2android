// Networking and peripheral system services (out of scope: online play, local wireless, friends, telemetry, SSL) as seen through newer SDKs: all made "unavailable", the game takes its offline branch.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
#include "hle.h"


// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "telemetry report save (user); nothing is sent, reports success", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "telemetry report save; nothing is sent, reports success", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "telemetry init: the original connects to the prepo service (through sm), the host has no service process; no-op (Engage calls it explicitly)", Result, h_prepo_Init, void) { return RES_OK; }
// Add the session ID / connection status to a report: on hardware these first connect to the prepo service for the session (Super Mario 3D World + Bowser's Fury calls them entering the save select); the host adds nothing and reports success
HLE_FORCE("_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc", "telemetry session ID: no service connection, nothing added, reports success", Result, h_prepo_AddSess, void *rep, const char *key) { (void)rep; (void)key; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "telemetry internet connection status: no service connection, nothing added, reports success", Result, h_prepo_AddNet, void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "telemetry immediate upload; nothing is sent, reports success", Result, h_prepo_Tx, void) { return RES_OK; }


// ---------- irsensor: the Joy-Con IR camera (Big Brain Academy: Brain vs. Brain's input layer makes one per controller). The host has none: every camera is unconnected,
// processors cannot run and report no states. The default-config getters and MakeRect only fill the game's structs and keep running in the SDK ----------
enum { IRS_UNCONNECTED = 2 };   // IrCameraStatus: 0 available, 1 unsupported, 2 unconnected (the game takes 0 as "available", main+0x497fd4)
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraHandleERKj", "IR camera handle of a controller (player number in the low byte)", uint32_t, h_irs_Handle, const uint32_t *npad) { return *npad & 0xff; }
HLE_FORCE("_ZN2nn8irsensor10InitializeERKNS0_14IrCameraHandleE", "IR camera (cut): nothing to initialize", void, h_irs_Init, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor8FinalizeERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Fini, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraStatusERKNS0_14IrCameraHandleE", "IR camera (cut): unconnected", int, h_irs_Status, const void *h) { (void)h; return IRS_UNCONNECTED; }
HLE_FORCE("_ZN2nn8irsensor18StopImageProcessorERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Stop, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor18RunMomentProcessorERKNS0_14IrCameraHandleERKNS0_21MomentProcessorConfigE", "IR camera (cut)", Result, h_irs_RunMoment, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor22RunClusteringProcessorERKNS0_14IrCameraHandleERKNS0_25ClusteringProcessorConfigE", "IR camera (cut)", Result, h_irs_RunCluster, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor15RunHandAnalysisERKNS0_14IrCameraHandleERKNS0_18HandAnalysisConfigE", "IR camera (cut)", Result, h_irs_RunHand, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_28ImageTransferProcessorConfigEPvm", "IR camera (cut)", Result, h_irs_RunImage,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_30ImageTransferProcessorExConfigEPvm", "IR camera (cut)", Result, h_irs_RunImageEx,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor24GetMomentProcessorStatesEPNS0_20MomentProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Moment,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor28GetClusteringProcessorStatesEPNS0_24ClusteringProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Cluster,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25GetHandAnalysisImageStateEPNS0_22HandAnalysisImageStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandImage,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetHandAnalysisSilhouetteStateEPNS0_27HandAnalysisSilhouetteStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandSil,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetImageTransferProcessorStateEPNS0_27ImageTransferProcessorStateEPvmRKNS0_14IrCameraHandleE", "IR camera (cut)", Result, h_irs_ImageState,
          void *s, void *b, size_t n, const void *h) { (void)s; (void)b; (void)n; (void)h; return RES_FAIL; }

// ---------- ssl: offline, creating a context fails ----------
HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL (cut)", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL (cut)", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL (cut)", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL (cut)", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

// ---------- nsd: offline (nifm is in hle_ldn.c) ----------
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "server environment name: retail lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }

// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "friends library allocator; no session is created, ignored", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "friend list is empty (cut)", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "friend list unavailable (cut)", Result, h_fr_ListAvail, uint8_t *out, const void *uid) { (void)uid; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "friend list (cut): the request fails at once", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "async context: always done", Result, h_fr_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "play history is not registered (cut)",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; int dm_lan_on(void); return dm_lan_on() ? RES_OK : RES_FAIL; }   // not stored; accepted with local play over the LAN on (hle_friends.c)

// ---------- ngc: newer SDK overloads (with bool). Always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "profanity filter init; the host has no word list", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "profanity check: no profanity in anything", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words; for (uint64_t i = 0; out && i < n; i++) out[i] = 0; return RES_OK;
}

// ---------- err: error display applet. Only logged, no UI is shown ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "error display applet; the host only logs it", void, h_err_ShowCode, uint64_t code) { LOG("game asks to show an error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "error display applet; the host only logs it", void, h_err_ShowVar, const void *v) { (void)v; LOG("game asks to show an error (ErrorResultVariant)"); }
HLE_FORCE("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", "add-on content version mismatch error applet; the host only logs it", void, h_err_AocVer, void) { LOG("game asks to show an error: add-on content version mismatch"); }
HLE_FORCE("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "game version mismatch error applet; the host only logs it", void, h_err_AppVer, void) { LOG("game asks to show an error: game version mismatch"); }
// Pokémon Legends: Z-A (SDK 19): the application's own error screen (an error code number and two messages in the argument) goes through the error applet as well
HLE_FORCE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet; the host only logs it", void, h_err_ShowApp, const void *arg) { (void)arg; LOG("game asks to show an application error (ApplicationErrorArg)"); }

// ---------- web: web page applet (online lobby). Unavailable ----------
HLE_FORCE("_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", "online lobby web applet (cut)", Result, h_web_Lobby, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowLobbyPage (cut)"); return RES_FAIL; }
// Tears of the Kingdom imports the general web page applet too (the game constructs the return value itself, so it stays as constructed)
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web page applet (cut)", Result, h_web_Page, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowWebPage (cut)"); return RES_FAIL; }
// Octopath Traveler 0 imports the offline HTML page applet too (pages shipped in the game's HTML document NCA, which the host does not extract)
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "offline HTML page applet (cut)", Result, h_web_Offline, void *rv, const void *arg) {
    (void)rv; (void)arg; LOG("web::ShowOfflineHtmlPage (cut)"); return RES_FAIL;
}


// ---------- bcat: news / data delivered from the server (Super Mario Odyssey). Initialize talks to the bcat service (an SDK abort without it); mounting the delivery cache fails,
// which the game treats as "nothing delivered" (main+0x6c9258: a failure skips the read unless the caller asked for an error dialog, which only logs here). Syncing needs the network, which nifm reports unavailable ----------
HLE_FORCE("_ZN2nn4bcat10InitializeEv", "data delivery init (connects to the bcat service); the host has none, no-op", void, h_bcat_Init, void) {}
HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "mount delivery cache: no delivery data, fails", Result, h_bcat_Mount, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "unmount delivery cache", void, h_bcat_Unmount, void) {}

// ---------- album: the capture album (Monster Hunter Rise saves its own photo-mode pictures there). Initialize connects to the capture service (capsrv, an abort in sm::Initialize at the start of
// nnMain, main+0x6bc5478); the host has no album, so saving fails and showing it does nothing. GetImageWidth / GetImageHeight only compute and keep running in the SDK ----------
HLE_FORCE("_ZN2nn5album10InitializeEv", "capture album init (capsrv); the host has no album, no-op", void, h_album_Init, void) {}
HLE_FORCE("_ZN2nn5album8FinalizeEv", "capture album", void, h_album_Fini, void) {}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save,
          const void *p, size_t n, int size, int orient, int report) { (void)p; (void)n; (void)size; (void)orient; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
// The SDK 7 overload, without an orientation (DRAGON QUEST XI S)
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save7,
          const void *p, size_t n, int size, int report) { (void)p; (void)n; (void)size; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_Share,
          const void *p, size_t n, int size, int orient, int report, const void *uid, const char *text) { (void)p; (void)n; (void)size; (void)orient; (void)report; (void)uid; (void)text; return RES_FAIL; }
// The SDK 20 overload that hands back a share ID, and the URL of the last post (Animal Crossing: New Horizons 3.0.3)
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_ShareId,
          void *id, const void *p, size_t n, int size, int report, const void *uid, const char *text) { (void)id; (void)p; (void)n; (void)size; (void)report; (void)uid; (void)text; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE", "URL of the last shared post: nothing was shared, fails", Result, h_album_ShareUrl, char *url, size_t n, const void *id) {
    (void)id; if (url && n) url[0] = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album14ShowAlbumFilesEv", "album applet: not shown", Result, h_album_Show, void) { return RES_FAIL; }
// Pokémon Legends: Z-A (SDK 19): a screenshot of the current frame, taken by the capture service
HLE_FORCE("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE", "screenshot of the current frame (capsrv): no album, fails", Result, h_album_SaveCur, int report) { (void)report; LOG("album::SaveCurrentScreenshot (no album)"); return RES_FAIL; }
