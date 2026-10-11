#include "shelf_options.h"

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

const ShelfOption pcsx_shelf_options[] = {
	{ "SYSTEM", "Region", "Select the console video region. Auto detects it from the game; forcing NTSC/PAL changes timing and refresh rate.",
	  "pcsx_rearmed_region", k_region_choices, 3, 0, 0, NULL },
	{ "SYSTEM", "BIOS", "Choose automatic BIOS selection or the built-in HLE BIOS. Auto uses a real BIOS when available. Takes effect on the next game start/restart.",
	  "pcsx_rearmed_bios", k_bios_choices, 2, 0, 0, NULL },
	{ "SYSTEM", "BIOS Boot Logo", "Controls whether the PlayStation BIOS boot animation is shown. Takes effect on the next game start/restart.",
	  "pcsx_rearmed_show_bios_bootlogo", k_bios_logo_choices, 3, 0, 0, NULL },
	{ "SYSTEM", "PSX CPU Clock", "Changes the emulated PlayStation CPU clock. Leave on Auto unless a game needs different CPU timing.",
	  "pcsx_rearmed_psxclock", k_auto_choice, 1, 30, 100, "%" },
	{ "SYSTEM", "Turbo CD", "Accelerates the emulated CD-ROM timing to reduce loading times. Some games rely on original drive timing and may misbehave, so disable it if a title has loading or streaming problems.",
	  "pcsx_rearmed_cd_turbo", k_toggle_choices, 2, 0, 0, NULL },
	{ "SYSTEM", "CD Read-Ahead", "Caches upcoming sectors on a background thread. This reduces storage stalls without changing PlayStation CD-ROM timing.",
	  "pcsx_rearmed_cd_readahead", k_cd_readahead_choices, 6, 0, 0, NULL },
	{ "VIDEO", "Aspect Ratio", "4:3 is the original output. 16:9 Stretched fills the screen by stretching 4:3. 16:9 Unstretched uses an experimental widescreen projection that preserves geometry proportions and can reveal extra horizontal content, but some games may show culling or background artifacts.",
	  "vita_aspect_ratio", k_aspect_choices, 3, 0, 0, NULL },
	{ "VIDEO", "Screen Borders", "Borderless removes vertical blanking outside the game's active display range. Auto and Game preserve the console timing canvas when a title intentionally uses it.",
	  "pcsx_rearmed_screen_centering", k_centering_choices, 3, 0, 0, NULL },
	{ "VIDEO", "Fractional Frame Rate", "Uses the console's fractional video timing when appropriate, such as 59.94 Hz instead of exactly 60 Hz.",
	  "pcsx_rearmed_fractional_framerate", k_fractional_choices, 3, 0, 0, NULL },
	{ "VIDEO", "Framebuffer Readout", "Changes when the displayed framebuffer is read. Early/Late can fix games with unusual framebuffer timing.",
	  "pcsx_rearmed_alt_flip", k_flip_choices, 3, 0, 0, NULL },
	{ "AUDIO", "Reverb", "Enables PlayStation SPU reverb effects.",
	  "pcsx_rearmed_spu_reverb", k_toggle_choices, 2, 0, 0, NULL },
	{ "AUDIO", "Interpolation", "Selects the SPU sample interpolation method. Higher-quality modes can slightly increase CPU cost.",
	  "pcsx_rearmed_spu_interpolation", k_interp_choices, 4, 0, 0, NULL },
	{ "AUDIO", "CD Audio", "Enables CD-DA audio tracks used by games that stream music directly from the disc.",
	  "pcsx_rearmed_nocdaudio", k_toggle_choices, 2, 0, 0, NULL },
	{ "AUDIO", "XA Decoding", "Enables XA audio decoding for streamed voices, music and sound effects stored on the disc.",
	  "pcsx_rearmed_noxadecoding", k_toggle_choices, 2, 0, 0, NULL },
};
const int pcsx_shelf_option_count = sizeof(pcsx_shelf_options) / sizeof(pcsx_shelf_options[0]);

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
