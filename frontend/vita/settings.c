#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "app_root.h"
#define SETTINGS_PATH APP_ROOT "/settings.cfg"
#define VALUE_SIZE 32

typedef struct VitaOptionEntry {
    const char *key;
    const char *default_value;
    char value[VALUE_SIZE];
} VitaOptionEntry;

static VitaOptionEntry g_options[] = {
    { "pcsx_rearmed_memcard1", "shared", {0} },
    { "pcsx_rearmed_memcard2", "none", {0} },
    { "pcsx_rearmed_bios", "auto", {0} },
    { "pcsx_rearmed_show_bios_bootlogo", "disabled", {0} },
    { "pcsx_rearmed_rgb32_output", "disabled", {0} },
    { "pcsx_rearmed_frameskip_type", "disabled", {0} },
    { "pcsx_rearmed_region", "auto", {0} },
    { "pcsx_rearmed_display_info", "disabled", {0} },
    { "pcsx_rearmed_vibration", "disabled", {0} },
    { "pcsx_rearmed_dithering", "enabled", {0} },
    { "pcsx_rearmed_neon_interlace_enable_v2", "auto", {0} },
    { "pcsx_rearmed_neon_enhancement_enable", "disabled", {0} },
    { "pcsx_rearmed_scale_hires", "disabled", {0} },
    { "pcsx_rearmed_gpu_thread_rendering", "disabled", {0} },
    { "pcsx_rearmed_spu_reverb", "enabled", {0} },
    { "pcsx_rearmed_spu_interpolation", "gaussian", {0} },
    { "pcsx_rearmed_nocdaudio", "enabled", {0} },
    { "pcsx_rearmed_noxadecoding", "enabled", {0} },
    { "pcsx_rearmed_spu_thread", "disabled", {0} },
    { "pcsx_rearmed_cd_turbo", "disabled", {0} },
    { "pcsx_rearmed_cd_readahead", "64", {0} },
    { "pcsx_rearmed_drc", "enabled", {0} },
    { "pcsx_rearmed_psxclock", "auto", {0} },
    { "pcsx_rearmed_fractional_framerate", "auto", {0} },
    { "pcsx_rearmed_alt_flip", "auto", {0} },
    { "pcsx_rearmed_screen_centering", "borderless", {0} },
    { "vita_aspect_ratio", "4:3", {0} },
    /* PCSX ReArcade: System 12 controls, PlayStation style (Tekken: square
     * left punch, triangle right punch, cross left kick, circle right kick). */
    { "pcsx_rearmed_arcade_square", "button1", {0} },
    { "pcsx_rearmed_arcade_triangle", "button2", {0} },
    { "pcsx_rearmed_arcade_cross", "button3", {0} },
    { "pcsx_rearmed_arcade_circle", "button4", {0} },
    { "pcsx_rearmed_arcade_l1", "button5", {0} },
    { "pcsx_rearmed_arcade_r1", "button6", {0} },
    { "pcsx_rearmed_arcade_l2", "button7", {0} },
    { "pcsx_rearmed_arcade_r2", "button8", {0} },
    { "pcsx_rearmed_arcade_select", "coin", {0} },
    { "pcsx_rearmed_arcade_start", "start", {0} },
    { "pcsx_rearmed_arcade_test", "disabled", {0} },
};

static bool g_dirty;

static VitaOptionEntry *find_option(const char *key)
{
    size_t i;
    if (!key)
        return NULL;
    for (i = 0; i < sizeof(g_options) / sizeof(g_options[0]); ++i)
        if (strcmp(g_options[i].key, key) == 0)
            return &g_options[i];
    return NULL;
}

static void reset_defaults(void)
{
    size_t i;
    for (i = 0; i < sizeof(g_options) / sizeof(g_options[0]); ++i) {
        snprintf(g_options[i].value, sizeof(g_options[i].value), "%s",
            g_options[i].default_value);
    }
}

void VitaSettings_Init(void)
{
    FILE *file;
    char line[160];

    reset_defaults();
    g_dirty = false;

    file = fopen(SETTINGS_PATH, "rb");
    if (!file)
        return;

    while (fgets(line, sizeof(line), file)) {
        char *eq;
        char *end;
        VitaOptionEntry *option;

        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';

        end = eq + strlen(eq);
        while (end > eq && (end[-1] == '\r' || end[-1] == '\n' ||
                            end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        option = find_option(line);
        if (!option || !eq[0])
            continue;
        snprintf(option->value, sizeof(option->value), "%s", eq);
    }

    fclose(file);
}

const char *VitaSettings_GetOption(const char *key)
{
    VitaOptionEntry *option = find_option(key);
    return option ? option->value : NULL;
}

int VitaSettings_SetOption(const char *key, const char *value)
{
    VitaOptionEntry *option = find_option(key);
    if (!option || !value || !value[0])
        return 0;
    if (strcmp(option->value, value) == 0)
        return 0;

    snprintf(option->value, sizeof(option->value), "%s", value);
    g_dirty = true;
    return 1;
}

int VitaSettings_Save(void)
{
    FILE *file;
    size_t i;

    file = fopen(SETTINGS_PATH, "wb");
    if (!file)
        return -1;

    for (i = 0; i < sizeof(g_options) / sizeof(g_options[0]); ++i)
        fprintf(file, "%s=%s\n", g_options[i].key, g_options[i].value);

    fclose(file);
    return 0;
}

bool VitaSettings_ConsumeDirty(void)
{
    const bool dirty = g_dirty;
    g_dirty = false;
    return dirty;
}
