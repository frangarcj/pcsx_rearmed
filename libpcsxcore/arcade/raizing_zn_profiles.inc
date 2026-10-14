/* Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a
 * sony/zn.cpp (BSD-3-Clause, smf/Philip Bennett). */
static const TpsMediaCheck raizing_media_bldyror2[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x910f3a8bu},
{1, 0x0, 0x200000, 1, 1, 0, 0xfa7602e1u},
{1, 0x200000, 0x200000, 1, 1, 0, 0x03465a69u},
{1, 0x800000, 0x400000, 1, 1, 0, 0x0e711461u},
{1, 0xc00000, 0x400000, 1, 1, 0, 0x0cf153f9u},
{1, 0x1000000, 0x400000, 1, 1, 0, 0xb71d955du},
{1, 0x1400000, 0x400000, 1, 1, 0, 0x89959ddeu},
{2, 0x0, 0x800, 1, 1, 0, 0x01b42397u},
{3, 0x1, 0x80000, 1, 2, 0, 0xe254dd8au},
{3, 0x0, 0x80000, 1, 2, 0, 0x10dc855bu},
{4, 0x0, 0x400000, 1, 1, 0, 0xb74cc4d1u},
{5, 0x0, 0x8, 1, 1, 0, 0xa7dd922eu},
{6, 0x0, 0x8, 1, 1, 0, 0x779b0bfdu}
};
static const TpsMediaCheck raizing_media_brvblade[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x69ffbcb4u},
{1, 0x0, 0x200000, 1, 1, 0, 0x97e12c63u},
{1, 0x200000, 0x200000, 1, 1, 0, 0xd9d40a34u},
{1, 0x800000, 0x400000, 1, 1, 0, 0x418535e0u},
{2, 0x0, 0x800, 1, 1, 0, 0xfe7f7d34u},
{3, 0x1, 0x80000, 1, 2, 0, 0x6408b5b2u},
{3, 0x0, 0x80000, 1, 2, 0, 0xc9df8ed9u},
{4, 0x0, 0x400000, 1, 1, 0, 0xcd052c02u},
{5, 0x0, 0x8, 1, 1, 0, 0x50dc8322u},
{6, 0x0, 0x8, 1, 1, 0, 0x56d917deu}
};
static const ZnProfile raizing_profiles[] = {
{"bldyror2", "coh1002e", "coh1002e", "fixed", "COH1002E", "RAIZING", "bldyror2", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x800, 0x100000, 0x400000},
{"brvblade", "coh1002m", "coh1002m", "fixed", "COH1002E", "RAIZING", "znt2p", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x1800000, 0x800, 0x100000, 0x400000}
};
static const TpsMediaCheck *const raizing_media[] = {raizing_media_bldyror2,raizing_media_brvblade};
static const size_t raizing_media_count[] = {sizeof(raizing_media_bldyror2) / sizeof(raizing_media_bldyror2[0]),sizeof(raizing_media_brvblade) / sizeof(raizing_media_brvblade[0])};
