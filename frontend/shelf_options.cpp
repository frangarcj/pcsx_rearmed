#include "shelf_options.h"

#include <vector>

/* Values and texts as in the PCSX AwO ROM selector, by Rinnegatamante. */
static const ShelfChoice k_auto_choice[] = { { "auto", "Auto" } };
static const ShelfChoice k_region_choices[] = {
	{ "auto", "Auto" }, { "NTSC", "NTSC" }, { "PAL", "PAL" }
};
static const ShelfChoice k_bios_choices[] = {
	{ "auto", "Auto" }, { "HLE", "HLE" }
};
static const ShelfChoice k_bios_logo_choices[] = {
	{ "disabled", "Off" }, { "enabled", "On" },
	{ "enabled_no_pcsx", "On, no PCSX logo" }
};
static const ShelfChoice k_fractional_choices[] = {
	{ "auto", "Auto" }, { "disabled", "Off" }, { "enabled", "On" }
};
static const ShelfChoice k_flip_choices[] = {
	{ "auto", "Auto" }, { "early", "Early" }, { "late", "Late" }
};
static const ShelfChoice k_centering_choices[] = {
	{ "auto", "Auto" }, { "game", "Game" }, { "borderless", "Borderless" }
};
static const ShelfChoice k_toggle_choices[] = {
	{ "disabled", "Off" }, { "enabled", "On" }
};
static const ShelfChoice k_cd_readahead_choices[] = {
	{ "0", "Off" }, { "12", "12 sectors" }, { "32", "32 sectors" },
	{ "64", "64 sectors" }, { "128", "128 sectors" }, { "256", "256 sectors" }
};
static const ShelfChoice k_interp_choices[] = {
	{ "off", "Off" }, { "simple", "Simple" },
	{ "gaussian", "Gaussian" }, { "cubic", "Cubic" }
};
static const ShelfChoice k_aspect_choices[] = {
	{ "4:3", "4:3" },
	{ "16:9_stretched", "16:9 Stretched" },
	{ "16:9_unstretched", "16:9 Unstretched" }
};

/* Where an option shows: the selector shows them all but the pause-only
 * ones; a game's pause menu, those that apply to it. SCOPE_SELECTOR: the
 * selector's only (the renderer picks the executable a game runs in). */
enum { SCOPE_DISC = 1, SCOPE_ARCADE = 2, SCOPE_ALL = 3, SCOPE_ARCADE_PAUSE = 4, SCOPE_SELECTOR = 8 };

static const ShelfChoice k_renderer_choices[] = {
	{ "vitagl", "vitaGL (HD)" }, { "neon", "NEON (exact)" }
};

static const ShelfChoice k_arcade_functions[] = {
	{ "button1", "Button 1" }, { "button2", "Button 2" }, { "button3", "Button 3" }, { "button4", "Button 4" },
	{ "button5", "Button 5" }, { "button6", "Button 6" }, { "button7", "Button 7" }, { "button8", "Button 8" },
	{ "coin", "Coin" }, { "start", "Start" }, { "service", "Service" }, { "none", "None" }
};
static const int k_arcade_function_count = sizeof(k_arcade_functions) / sizeof(k_arcade_functions[0]);

static const struct
{
	int scope;
	ShelfOption option;
} k_options[] = {
	{ SCOPE_DISC, { "PLAYSTATION", "Region", "Select the console video region. Auto detects it from the game; forcing NTSC/PAL changes timing and refresh rate.",
	  "pcsx_rearmed_region", k_region_choices, 3, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION", "BIOS", "Choose automatic BIOS selection or the built-in HLE BIOS. Auto uses a real BIOS when available. Takes effect on the next game start/restart.",
	  "pcsx_rearmed_bios", k_bios_choices, 2, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION", "BIOS Boot Logo", "Controls whether the PlayStation BIOS boot animation is shown. Takes effect on the next game start/restart.",
	  "pcsx_rearmed_show_bios_bootlogo", k_bios_logo_choices, 3, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION", "PSX CPU Clock", "Changes the emulated PlayStation CPU clock. Leave on Auto unless a game needs different CPU timing.",
	  "pcsx_rearmed_psxclock", k_auto_choice, 1, 30, 100, "%" } },
	{ SCOPE_DISC, { "PLAYSTATION", "Turbo CD", "Accelerates the emulated CD-ROM timing to reduce loading times. Some games rely on original drive timing and may misbehave, so disable it if a title has loading or streaming problems.",
	  "pcsx_rearmed_cd_turbo", k_toggle_choices, 2, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION", "CD Read-Ahead", "Caches upcoming sectors on a background thread. This reduces storage stalls without changing PlayStation CD-ROM timing.",
	  "pcsx_rearmed_cd_readahead", k_cd_readahead_choices, 6, 0, 0, NULL } },
	{ SCOPE_SELECTOR, { "VIDEO", "Renderer", "vitaGL draws on the GPU at the screen's resolution, with antialiasing. NEON draws on the CPU, exactly as the PlayStation GPU does, at its own resolution. Applies to the next game started.",
	  "pcsx_rearcade_renderer", k_renderer_choices, 2, 0, 0, NULL } },
	{ SCOPE_ALL, { "VIDEO", "Aspect Ratio", "4:3 is the original output. 16:9 Stretched fills the screen by stretching 4:3. 16:9 Unstretched uses an experimental widescreen projection that preserves geometry proportions and can reveal extra horizontal content, but some games may show culling or background artifacts.",
	  "vita_aspect_ratio", k_aspect_choices, 3, 0, 0, NULL } },
	{ SCOPE_ALL, { "VIDEO", "Screen Borders", "Borderless removes vertical blanking outside the game's active display range. Auto and Game preserve the console timing canvas when a title intentionally uses it.",
	  "pcsx_rearmed_screen_centering", k_centering_choices, 3, 0, 0, NULL } },
	{ SCOPE_ALL, { "VIDEO", "Fractional Frame Rate", "Uses the console's fractional video timing when appropriate, such as 59.94 Hz instead of exactly 60 Hz.",
	  "pcsx_rearmed_fractional_framerate", k_fractional_choices, 3, 0, 0, NULL } },
	{ SCOPE_ALL, { "VIDEO", "Framebuffer Readout", "Changes when the displayed framebuffer is read. Early/Late can fix games with unusual framebuffer timing.",
	  "pcsx_rearmed_alt_flip", k_flip_choices, 3, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION AUDIO", "Reverb", "Enables PlayStation SPU reverb effects.",
	  "pcsx_rearmed_spu_reverb", k_toggle_choices, 2, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION AUDIO", "Interpolation", "Selects the SPU sample interpolation method. Higher-quality modes can slightly increase CPU cost.",
	  "pcsx_rearmed_spu_interpolation", k_interp_choices, 4, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION AUDIO", "CD Audio", "Enables CD-DA audio tracks used by games that stream music directly from the disc.",
	  "pcsx_rearmed_nocdaudio", k_toggle_choices, 2, 0, 0, NULL } },
	{ SCOPE_DISC, { "PLAYSTATION AUDIO", "XA Decoding", "Enables XA audio decoding for streamed voices, music and sound effects stored on the disc.",
	  "pcsx_rearmed_noxadecoding", k_toggle_choices, 2, 0, 0, NULL } },
	/* System 12 controls: what each pad button does (libretro.c). */
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "Square", "What the Square button does on an arcade board. Buttons 1-4 are the four attack buttons (Tekken: left punch, right punch, left kick, right kick).",
	  "pcsx_rearmed_arcade_square", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "Triangle", "What the Triangle button does on an arcade board.",
	  "pcsx_rearmed_arcade_triangle", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "Cross", "What the Cross button does on an arcade board.",
	  "pcsx_rearmed_arcade_cross", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "Circle", "What the Circle button does on an arcade board.",
	  "pcsx_rearmed_arcade_circle", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "L", "What the L button does on an arcade board.",
	  "pcsx_rearmed_arcade_l1", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "R", "What the R button does on an arcade board.",
	  "pcsx_rearmed_arcade_r1", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "Select", "What the Select button does on an arcade board: Coin inserts a credit. Start + Select still opens the pause menu.",
	  "pcsx_rearmed_arcade_select", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "L2 (PS TV)", "What L2 does on an arcade board, on a PS TV or a Vita with rear touch mapped to it.",
	  "pcsx_rearmed_arcade_l2", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE, { "ARCADE CONTROLS", "R2 (PS TV)", "What R2 does on an arcade board, on a PS TV or a Vita with rear touch mapped to it.",
	  "pcsx_rearmed_arcade_r2", k_arcade_functions, k_arcade_function_count, 0, 0, NULL } },
	{ SCOPE_ARCADE_PAUSE, { "ARCADE", "Test Switch", "The board's test switch: On opens the game's test menu (input and sound tests, settings, bookkeeping). Turn it Off and leave the menu to play. Off again for every new game.",
	  "pcsx_rearmed_arcade_test", k_toggle_choices, 2, 0, 0, NULL } },
};

const ShelfOption *pcsx_shelf_options(int context, int *count)
{
	static std::vector<ShelfOption> tables[3];
	std::vector<ShelfOption> &table = tables[context];
	if (table.empty())
		for (const auto &entry : k_options)
		{
			const bool shown = context == PCSX_OPTIONS_SELECTOR ? !(entry.scope & SCOPE_ARCADE_PAUSE)
			                 : context == PCSX_OPTIONS_DISC     ? (entry.scope & SCOPE_DISC) != 0
			                                                    : (entry.scope & (SCOPE_ARCADE | SCOPE_ARCADE_PAUSE)) != 0;
			if (shown)
				table.push_back(entry.option);
		}
	*count = (int)table.size();
	return table.data();
}

std::string pcsx_shelf_about(const char *version)
{
	std::string text = "# PCSX ReArcade";
	if (version && *version)
		text += std::string(" ") + version;
	text +=
		"\n"
		"Namco System 12 arcade games and PlayStation discs on the PS Vita, by "
		"Francisco José García García (frangarcj).\n"
		"\n"
		"# Built on\n"
		"PCSX-ReARMed by notaz and contributors, from PCSX-Reloaded, PCSX-df and PCSX: Ari64's dynamic "
		"recompiler, Exophase's NEON GPU, the P.E.Op.S. SPU and the BIOS HLE.\n"
		"PCSX AwO by Rinnegatamante: the PS Vita frontend, the ROM selector, options and save states, "
		"the disc of the game selector, and vitaGL.\n"
		"The native PS1 renderer by aalhendi, from PsyCross (REDRIVER2 Project, MIT).\n"
		"\n"
		"# Arcade hardware\n"
		"MAME and its developers: this port follows MAME's Namco System 12 driver, its devices and its "
		"romset definitions (BSD-3-Clause).\n"
		"\n"
		"# Libraries\n"
		"VitaSDK, vitaGL, SDL, Dear ImGui (Omar Cornut), libpng, zlib, libchdr, and the Roboto font "
		"(Christian Robertson, Apache 2.0).\n"
		"\n"
		"# Licence\n"
		"GNU GPL version 2. Source: github.com/frangarcj/pcsx_rearmed\n"
		"Not affiliated with Namco, Bandai Namco or Sony. No game data is included: use your own romsets "
		"and discs.\n";
	return text;
}
