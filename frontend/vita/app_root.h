/* The app's data folder on the Vita. PCSX ReArcade (PCSX_REARCADE, the
 * arcade-shelf build) keeps its own; validation builds keep PCSX AwO's,
 * which the measurement tools use. */
#ifndef PCSX_VITA_APP_ROOT_H
#define PCSX_VITA_APP_ROOT_H

#ifdef PCSX_REARCADE
#define APP_ROOT "ux0:data/pcsx-rearcade"
#else
#define APP_ROOT "ux0:data/pcsx-rearmed"
#endif

#endif
