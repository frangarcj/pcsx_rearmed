/* Exercise the production shelf with prepared profiles, without ROM data. */
#include "../frontend/arcade_catalog.h"
#include "../frontend/arcade_profile.h"
#include "../libpcsxcore/arcade/arcade_romset.h"
#include "../libpcsxcore/arcade/system12_romset.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

/* Keep the native zip scanner outside this directory-profile fixture. Its
 * CRC/merged/split/media behaviour has tests/test_system12_romset.py. */
extern "C" unsigned system12_romset_count(void) { return 0; }
extern "C" const system12_romset_info *system12_romset_at(unsigned) { return nullptr; }
extern "C" int system12_romset_scan(const char *, unsigned char *) { return 0; }
extern "C" int system12_romset_scan_index(const mame_romindex *, unsigned char *) { return 0; }
extern "C" unsigned arcade_romset_count(void) { return 0; }
extern "C" const arcade_romset_info *arcade_romset_at(unsigned) { return nullptr; }
extern "C" int arcade_romset_scan_index(const mame_romindex *, unsigned char *) { return 0; }
extern "C" mame_romindex *mame_romindex_open(const char *) { return nullptr; }
extern "C" void mame_romindex_close(mame_romindex *) {}

static void put(const std::string &path, const std::string &text = "")
{ std::ofstream f(path); assert(f); f << text; }

static void profile(const std::string &root, const char *directory, const char *format,
    const char *set, const char *files)
{
    const std::string path = root + "/" + directory;
    assert(!mkdir(path.c_str(), 0700));
    put(path + "/board.ini", std::string("format=") + format + "\nmame_set=" + set + "\n");
    std::string names = files;
    size_t start = 0;
    while (start < names.size()) {
        size_t end = names.find('|', start);
        if (end == std::string::npos) end = names.size();
        put(path + "/" + names.substr(start, end - start)); start = end + 1;
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::string root = argv[1];
    const char *s11_files = "main.bin|banked.bin|c76.bin|c76bios.bin|wave.bin";
    const char *zn_files = "main.bin|banked.bin|country.bin|audiocpu.bin|qsound_samples.bin|qsound_dsp.bin|cat702_key0.bin|cat702_key1.bin";
    profile(root, "namco10", "pcsx-aow-system10-v1", "mrdrilr2", "main.bin|rawflash.bin|datarom.bin");
    profile(root, "namco11", "pcsx-aow-system11-v1", "tekken", s11_files);
    profile(root, "namco11-clone", "pcsx-aow-system11-v1", "tekkenb", s11_files);
    profile(root, "capcom", "pcsx-aow-capcom-zn-v1", "sfex", zn_files);
    profile(root, "incomplete", "pcsx-aow-system11-v1", "tekken2", "main.bin");
    profile(root, "wrong-family", "pcsx-aow-system10-v1", "tekken2", s11_files);
    profile(root, "unknown", "pcsx-aow-system573-v1", "hyperbbc", "main.bin");
    profile(root, "duplicate-set", "pcsx-aow-system11-v1", "tekken2", s11_files);
    put(root + "/duplicate-set/board.ini", "format=pcsx-aow-system11-v1\nmame_set=tekken2\nmame_set=tekken\n");
    ArcadeProfile id;
    assert(arcade_profile_read((root + "/namco10").c_str(), &id) && id.kind == ARCADE_PROFILE_SYSTEM10);
    assert(!arcade_profile_read((root + "/unknown").c_str(), &id));
    assert(!arcade_profile_read((root + "/duplicate-set").c_str(), &id));
    ArcadeCatalog catalog;
    catalog.build(root, false);
    assert(catalog.count() == 3);
    bool s10 = false, s11 = false, zn = false;
    for (int i = 0; i < catalog.count(); i++) {
        const ShelfGame &g = catalog.games()[i];
        if (!strcmp(g.system, "Namco System 10")) { s10 = true; assert(!strcmp(g.versions[0].launch, (root + "/namco10").c_str())); }
        if (!strcmp(g.system, "Namco System 11")) { s11 = true; assert(g.version_count == 2); assert(!strcmp(g.versions[0].id, "tekken")); }
        if (!strcmp(g.system, "Capcom ZN")) zn = true;
    }
    assert(s10 && s11 && zn);
    catalog.add_disc("disc1.cue", "Tekken (Europe) (Disc 1)", "tekken");
    assert(catalog.count() == 4);
    assert(!strcmp(catalog.games()[3].system, "PlayStation"));
    catalog.build(root, false); assert(catalog.count() == 3);
    puts("Prepared arcade catalogue: families, grouping, launches, incomplete/invalid profiles and disc coexistence PASS");
}
