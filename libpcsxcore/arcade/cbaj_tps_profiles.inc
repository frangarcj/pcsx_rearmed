/* Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a
 * sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */
static const TpsMediaCheck cbaj_media_cbaj[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x69ffbcb4u},
{1, 0x1, 0x80000, 1, 2, 0, 0x814f8b4bu},
{1, 0x0, 0x80000, 1, 2, 0, 0x89286229u},
{1, 0x400000, 0x400000, 1, 1, 0, 0x3db68beau},
{1, 0x800000, 0x400000, 1, 1, 0, 0x481040bcu},
{1, 0xc00000, 0x400000, 1, 1, 0, 0x858f116cu},
{1, 0x1000000, 0x400000, 1, 1, 0, 0x3576ea2au},
{1, 0x1400000, 0x400000, 1, 1, 0, 0x551c4b29u},
{1, 0x1800000, 0x400000, 1, 1, 0, 0x7da453dau},
{1, 0x1c00000, 0x400000, 1, 1, 0, 0x833cb18bu},
{1, 0x2000000, 0x400000, 1, 1, 0, 0x3b64ce9eu},
{1, 0x2400000, 0x400000, 1, 1, 0, 0x57cc482eu},
{3, 0x0, 0x40000, 1, 1, 0, 0x92b02ad2u},
{4, 0x0, 0x400000, 1, 1, 0, 0xafb05d6du},
{4, 0x400000, 0x400000, 1, 1, 0, 0xf12b3db9u},
{5, 0x0, 0x8, 1, 1, 0, 0x50dc8322u},
{6, 0x0, 0x8, 1, 1, 0, 0x8be79633u},
{7, 0x0, 0x117, 1, 1, 0, 0xcf8ebc23u},
{7, 0x200, 0x117, 1, 1, 0, 0x098662c1u},
{7, 0x400, 0x117, 1, 1, 0, 0xf9c92db7u},
{7, 0x600, 0x117, 1, 1, 0, 0xe87179efu},
{7, 0x800, 0x117, 1, 1, 0, 0xf32a1803u},
{7, 0xa00, 0x117, 1, 1, 0, 0x40e1f6f2u}
};
static const ZnProfile cbaj_profiles[] = {
{"cbaj", "coh1002m", "coh1002m", "fixed", "COH1002M", "CBAJ", "zn1p", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2800000, 0, 0x40000, 0x800000}
};
static const TpsMediaCheck *const cbaj_media[] = {cbaj_media_cbaj};
static const size_t cbaj_media_count[] = {sizeof(cbaj_media_cbaj) / sizeof(cbaj_media_cbaj[0])};
