/* Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */
static const TpsMediaCheck primal_media_primrag2[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x45e8a4b4u},
{1, 0x1, 0x80000, 1, 2, 0, 0x3ee39e4fu},
{1, 0x0, 0x80000, 1, 2, 0, 0xc86450cdu},
{1, 0x100001, 0x80000, 1, 2, 0, 0x3681516cu},
{1, 0x100000, 0x80000, 1, 2, 0, 0x4b24bd54u},
{2, 0x0, 0x800, 1, 1, 0, 0x62c21283u},
{5, 0x0, 0x8, 1, 1, 0, 0x1cac067eu},
{6, 0x0, 0x8, 1, 1, 0, 0x583e8e1cu}
};
static const ZnProfile primal_profiles[] = {
{"primrag2", "coh1000w", "coh1000w", "fixed", "COH1000W", "PRIMAL", "primrag2", 67737600, 53693175, 0x800000, 0x200000, 0x80000, 0x200000, 0x800, 0, 0}
};
static const TpsMediaCheck *const primal_media[] = {primal_media_primrag2};
static const size_t primal_media_count[] = {sizeof(primal_media_primrag2)/sizeof(primal_media_primrag2[0])};
