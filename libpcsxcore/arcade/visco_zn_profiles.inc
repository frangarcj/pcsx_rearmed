/* Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a
 * sony/zn.cpp (BSD-3-Clause, smf/Philip Bennett). */
static const TpsMediaCheck visco_media_aerofgts[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x5ff165f3u},
{1, 0x0, 0x400000, 1, 1, 0, 0xa93f6feeu},
{1, 0x400000, 0x400000, 1, 1, 0, 0x9f584ef7u},
{1, 0x800000, 0x400000, 1, 1, 0, 0x652e9c78u},
{1, 0xc00000, 0x400000, 1, 1, 0, 0xc4ef1424u},
{1, 0x1000000, 0x400000, 1, 1, 0, 0x2551d816u},
{1, 0x1400000, 0x400000, 1, 1, 0, 0x21b401bcu},
{2, 0x0, 0x80000, 1, 1, 0, 0xd15a4904u},
{2, 0x80000, 0x80000, 1, 1, 0, 0xd6625176u},
{2, 0x100000, 0x80000, 1, 1, 0, 0xdf5ba2f7u},
{2, 0x180000, 0x80000, 1, 1, 0, 0x8711574au},
{2, 0x200000, 0x80000, 1, 1, 0, 0x670d9b27u},
{5, 0x0, 0x8, 1, 1, 0, 0x1ce271dfu},
{6, 0x0, 0x8, 1, 1, 0, 0x0d2001b4u}
};
static const TpsMediaCheck visco_media_aerofgtst[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x5ff165f3u},
{1, 0x0, 0x400000, 1, 1, 0, 0xa93f6feeu},
{1, 0x400000, 0x400000, 1, 1, 0, 0x9f584ef7u},
{1, 0x800000, 0x400000, 1, 1, 0, 0x652e9c78u},
{1, 0xc00000, 0x400000, 1, 1, 0, 0xc4ef1424u},
{1, 0x1000000, 0x400000, 1, 1, 0, 0x2551d816u},
{1, 0x1400000, 0x400000, 1, 1, 0, 0x21b401bcu},
{2, 0x0, 0x80000, 1, 1, 0, 0x453dd514u},
{2, 0x80000, 0x80000, 1, 1, 0, 0x24257295u},
{2, 0x100000, 0x80000, 1, 1, 0, 0xdf5ba2f7u},
{2, 0x180000, 0x80000, 1, 1, 0, 0xdf638f92u},
{2, 0x200000, 0x80000, 1, 1, 0, 0x1294aa24u},
{5, 0x0, 0x8, 1, 1, 0, 0x1ce271dfu},
{6, 0x0, 0x8, 1, 1, 0, 0x0d2001b4u}
};
static const TpsMediaCheck visco_media_sncwgltd[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x5ff165f3u},
{1, 0x0, 0x400000, 1, 1, 0, 0xa93f6feeu},
{1, 0x400000, 0x400000, 1, 1, 0, 0x9f584ef7u},
{1, 0x800000, 0x400000, 1, 1, 0, 0x652e9c78u},
{1, 0xc00000, 0x400000, 1, 1, 0, 0xc4ef1424u},
{1, 0x1000000, 0x400000, 1, 1, 0, 0x2551d816u},
{1, 0x1400000, 0x400000, 1, 1, 0, 0x21b401bcu},
{2, 0x0, 0x80000, 1, 1, 0, 0x458f14aau},
{2, 0x80000, 0x80000, 1, 1, 0, 0x8233dd1eu},
{2, 0x100000, 0x80000, 1, 1, 0, 0xdf5ba2f7u},
{2, 0x180000, 0x80000, 1, 1, 0, 0xe8145f2bu},
{2, 0x200000, 0x80000, 1, 1, 0, 0x605c9370u},
{5, 0x0, 0x8, 1, 1, 0, 0x1ce271dfu},
{6, 0x0, 0x8, 1, 1, 0, 0x0d2001b4u}
};
static const ZnProfile visco_profiles[] = {
{"aerofgts", "coh1002v", "coh1002v", "fixed", "COH1002V", "VISCO", "aerofgts", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x280000, 0, 0},
{"aerofgtst", "aerofgts", "coh1002v", "fixed", "COH1002V", "VISCO", "aerofgts", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x280000, 0, 0},
{"sncwgltd", "aerofgts", "coh1002v", "fixed", "COH1002V", "VISCO", "aerofgts", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x280000, 0, 0}
};
static const TpsMediaCheck *const visco_media[] = {visco_media_aerofgts,visco_media_aerofgtst,visco_media_sncwgltd};
static const size_t visco_media_count[] = {sizeof(visco_media_aerofgts) / sizeof(visco_media_aerofgts[0]),sizeof(visco_media_aerofgtst) / sizeof(visco_media_aerofgtst[0]),sizeof(visco_media_sncwgltd) / sizeof(visco_media_sncwgltd[0])};
