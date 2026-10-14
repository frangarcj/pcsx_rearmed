/* Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a
 * sony/zn.cpp (BSD-3-Clause, smf/R. Belmont). */
static const TpsMediaCheck atlus_media_hvnsgate[] = {
{0, 0x0, 0x80000, 1, 1, 0, 0x6721146bu},
{1, 0x1, 0x80000, 1, 2, 0, 0xe820136fu},
{1, 0x0, 0x80000, 1, 2, 0, 0x11bfa89bu},
{1, 0x100000, 0x400000, 1, 1, 0, 0x46411f67u},
{1, 0x500000, 0x400000, 1, 1, 0, 0x85289345u},
{1, 0x900000, 0x400000, 1, 1, 0, 0x19e558b5u},
{1, 0xd00000, 0x400000, 1, 1, 0, 0x748f936eu},
{1, 0x1100000, 0x200000, 1, 1, 0, 0xac8e53bdu},
{3, 0x1, 0x20000, 1, 2, 0, 0x18523e85u},
{3, 0x0, 0x20000, 1, 2, 0, 0x7eef7e68u},
{4, 0x400000, 0x200000, 1, 1, 0, 0x74469a15u},
{4, 0x600000, 0x200000, 1, 1, 0, 0x443ade73u},
{5, 0x0, 0x8, 1, 1, 0, 0x181668b3u},
{6, 0x0, 0x8, 1, 1, 0, 0x55fe469eu}
};
static const ZnFillCheck atlus_fills[] = {{4, 0, 0x400000, 0xff}};
static const ZnProfile atlus_profiles[] = {
{"hvnsgate", "coh1001l", "coh1001l", "fixed", "COH1001L", "ATLUS", "zn2p", 67737600, 53693175, 0x400000, 0x200000, 0x80000, 0x2000000, 0, 0x40000, 0x800000}
};
static const TpsMediaCheck *const atlus_media[] = {atlus_media_hvnsgate};
static const size_t atlus_media_count[] = {sizeof(atlus_media_hvnsgate) / sizeof(atlus_media_hvnsgate[0])};
