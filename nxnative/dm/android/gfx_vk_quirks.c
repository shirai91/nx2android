// GPU driver quirks (gfx_vk_quirks.h): which GPU and driver the device has, and the table of workarounds for it.
// Add a row when a fix is needed by one vendor's hardware or one driver only; keep it off for the others until a device of theirs shows it is needed.
#include "gfx_vk.h"   // the Vulkan declarations, then gfx_vk_quirks.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VENDOR_QUALCOMM = 0x5143, VENDOR_ARM = 0x13B5, VENDOR_IMG = 0x1010, VENDOR_SAMSUNG = 0x144D };

unsigned char g_vk_quirks[VQ_COUNT];
vk_gpu g_vk_gpu;

static int adreno(const vk_gpu *g) { return g->vendor == VENDOR_QUALCOMM; }   // both drivers: Qualcomm's own and Mesa's Turnip

static const struct {
    enum vk_quirk q; const char *name, *setting; int (*match)(const vk_gpu *); const char *why;
} ROWS[] = {
    // found with NEO's menu wipe and Xenoblade Chronicles 3's cloud dome
    {VQ_LRZ_TOUCH, "lrz_touch", "DM_VK_QUIRK_LRZ_TOUCH", adreno,
     "Adreno's low-resolution Z keeps depth from before a command-buffer cut or a pass switch: draws behind objects vanish"},
    // written for NieR:Automata
    {VQ_GS_SPLIT, "gs_split", "DM_VK_QUIRK_GS_SPLIT", adreno,
     "geometry-shader draws split into vertex and fragment passes: written and checked on Adreno only"},
};
_Static_assert(sizeof ROWS / sizeof *ROWS == VQ_COUNT, "one row a quirk");

const char *vk_driver_kind(void) {
    switch (g_vk_gpu.driver) {
    case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: return "qualcomm";
    case VK_DRIVER_ID_MESA_TURNIP: return "turnip";
    case VK_DRIVER_ID_ARM_PROPRIETARY: return "mali";
    case VK_DRIVER_ID_MESA_PANVK: return "panvk";
    case VK_DRIVER_ID_SAMSUNG_PROPRIETARY: return "xclipse";
    case VK_DRIVER_ID_IMAGINATION_PROPRIETARY: return "powervr";
    default: return g_vk_gpu.vendor == VENDOR_QUALCOMM ? "adreno (driver not named)" : "unknown";
    }
}

void vk_quirks_init(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *p) {
    vk_gpu *g = &g_vk_gpu; memset(g, 0, sizeof *g);
    g->vendor = p->vendorID; g->device = p->deviceID; g->version = p->driverVersion; g->api = p->apiVersion;
    // VkPhysicalDeviceDriverProperties is core from Vulkan 1.2; on 1.1 only with VK_KHR_driver_properties (chaining it otherwise is a valid-usage error)
    int has_dp = p->apiVersion >= VK_API_VERSION_1_2;
    if (!has_dp && p->apiVersion >= VK_API_VERSION_1_1) {
        uint32_t m = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &m, NULL);
        VkExtensionProperties *e = calloc(m ? m : 1, sizeof *e);
        if (e) { vkEnumerateDeviceExtensionProperties(pd, NULL, &m, e); for (uint32_t i = 0; i < m && !has_dp; i++) has_dp = !strcmp(e[i].extensionName, "VK_KHR_driver_properties"); free(e); }
    }
    if (has_dp) {
        VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
        vkGetPhysicalDeviceProperties2(pd, &p2);
        g->driver = (int)dp.driverID; snprintf(g->name, sizeof g->name, "%s", dp.driverName); snprintf(g->info, sizeof g->info, "%s", dp.driverInfo);
    }
    // a Turnip that does not give its driverID: by its driverName, else a device name "Turnip ..." (Mesa 26.2's names the GPU "Adreno (TM) 830" with Qualcomm's vendor id)
    if (!g->driver && (!strncmp(g->name, "turnip", 6) || !strncmp(p->deviceName, "Turnip", 6))) g->driver = VK_DRIVER_ID_MESA_TURNIP;
    char list[512]; size_t n = 0; list[0] = 0;
    for (size_t i = 0; i < sizeof ROWS / sizeof *ROWS; i++) {
        int on = ROWS[i].match(g); const char *e = getenv(ROWS[i].setting), *how = "";
        if (e && *e) { on = atoi(e) != 0; how = " (forced)"; }
        g_vk_quirks[ROWS[i].q] = (unsigned char)on;
        n += (size_t)snprintf(list + n, n < sizeof list ? sizeof list - n : 0, "%s%s %s%s", i ? ", " : "", ROWS[i].name, on ? "on" : "off", how);
        if (n >= sizeof list) n = sizeof list - 1;
    }
    VKLOG("driver %s (%s, id %d, info \"%s\"); quirks: %s", vk_driver_kind(), g->name[0] ? g->name : "no name", g->driver, g->info, list);
}
