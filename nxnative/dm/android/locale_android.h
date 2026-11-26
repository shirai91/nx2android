#pragma once
#include <string.h>

static inline int android_language_list_has(const char *list, const char *code) {
    size_t n = strlen(code);
    if (!n) return 0;
    for (const char *p = list; (p = strstr(p, code)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

static inline const char *android_locale_part(char **rest) {
    char *part = *rest;
    if (!part) return NULL;
    char *dash = strchr(part, '-');
    if (dash) *dash++ = '\0';
    *rest = dash;
    return part;
}

// Locale.toLanguageTag() returns canonical case, with a language, optional script and optional region.
static inline const char *android_game_language(const char *tag, const char *supported) {
    static const char *const codes[] = {"en-US", "en-GB", "ja", "fr", "de", "es-419", "es", "it", "nl",
                                       "fr-CA", "pt", "ru", "ko", "zh-Hant", "zh-Hans", "pt-BR"};
    char locale[64] = {0};
    strncpy(locale, tag, sizeof locale - 1);
    char *rest = locale;
    const char *lang = android_locale_part(&rest);
    const char *part = android_locale_part(&rest);
    const char *script = part && strlen(part) == 4 ? part : "";
    const char *region = *script ? android_locale_part(&rest) : part;
    if (!region) region = "";

    const char *want = "en-US", *near = NULL;
    if (!strcmp(lang, "zh")) {
        int traditional = !strcmp(script, "Hant") || (!*script && android_language_list_has("HK MO TW", region));
        want = traditional ? "zh-Hant" : "zh-Hans";
    } else if (!strcmp(lang, "en")) {
        int british = android_language_list_has("GB AU NZ IE ZA", region);
        want = british ? "en-GB" : "en-US"; near = british ? "en-US" : "en-GB";
    } else if (!strcmp(lang, "fr")) {
        int canadian = !strcmp(region, "CA");
        want = canadian ? "fr-CA" : "fr"; near = canadian ? "fr" : "fr-CA";
    } else if (!strcmp(lang, "es")) {
        int american = android_language_list_has("419 AR BO BR BZ CL CO CR CU DO EC GT HN MX NI PA PE PR PY SV US UY VE", region);
        want = american ? "es-419" : "es"; near = american ? "es" : "es-419";
    } else if (!strcmp(lang, "pt")) {
        int brazilian = !strcmp(region, "BR");
        want = brazilian ? "pt-BR" : "pt"; near = brazilian ? "pt" : "pt-BR";
    } else {
        for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) if (!strcmp(lang, codes[i])) want = codes[i];
    }
    if (!supported || !*supported || android_language_list_has(supported, want)) return want;
    if (near && android_language_list_has(supported, near)) return near;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) if (android_language_list_has(supported, codes[i])) return codes[i];
    return "en-US";
}
