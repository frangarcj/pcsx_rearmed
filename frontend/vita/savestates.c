#include "savestates.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>

#define APP_ROOT "ux0:data/pcsx-rearmed"
#define STATE_DIR APP_ROOT "/savestates"
#define SLOT_PATH STATE_DIR "/slot.cfg"
#define STATE_MAGIC "PCSXST01"
#define STATE_VERSION 1u

extern size_t retro_serialize_size(void);
extern bool retro_serialize(void *data, size_t size);
extern bool retro_unserialize(const void *data, size_t size);

typedef struct VitaStateHeader {
    char magic[8];
    uint32_t version;
    uint32_t data_size;
    uint32_t reserved[4];
} VitaStateHeader;

static int g_state_slot;

static void make_game_key(const char *game_path, char *out, size_t out_size)
{
    const char *base;
    const char *dot;
    size_t i, len;

    if (!out || out_size == 0)
        return;

    out[0] = '\0';
    if (!game_path || !game_path[0]) {
        snprintf(out, out_size, "game");
        return;
    }

    base = strrchr(game_path, '/');
    base = base ? base + 1 : game_path;
    dot = strrchr(base, '.');
    len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (len > 96)
        len = 96;

    for (i = 0; i < len && i + 1 < out_size; ++i) {
        unsigned char ch = (unsigned char)base[i];
        if (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            out[i] = '_';
        else
            out[i] = (char)ch;
    }
    out[i] = '\0';

    if (!out[0])
        snprintf(out, out_size, "game");
}

static void make_state_path(const char *game_path, int slot, char *out, size_t out_size)
{
    char key[128];
    make_game_key(game_path, key, sizeof(key));
    snprintf(out, out_size, STATE_DIR "/%s.slot%d.state", key, slot);
}

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *ptr = (const uint8_t *)data;
    size_t done = 0;

    while (done < size) {
        int written = sceIoWrite(fd, ptr + done, size - done);
        if (written <= 0)
            return -1;
        done += (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t size)
{
    uint8_t *ptr = (uint8_t *)data;
    size_t done = 0;

    while (done < size) {
        int read = sceIoRead(fd, ptr + done, size - done);
        if (read <= 0)
            return -1;
        done += (size_t)read;
    }
    return 0;
}

static int alloc_state_buffer(size_t size, SceUID *uid, void **base)
{
    const size_t alloc_size = (size + 0xfff) & ~(size_t)0xfff;

    *uid = sceKernelAllocMemBlock("pcsx-savestate",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, alloc_size, NULL);
    if (*uid < 0)
        return -1;
    if (sceKernelGetMemBlockBase(*uid, base) < 0) {
        sceKernelFreeMemBlock(*uid);
        *uid = -1;
        return -1;
    }
    return 0;
}

void VitaSaveStates_Init(void)
{
    char buf[16];
    int fd;
    int read;

    sceIoMkdir(STATE_DIR, 0777);
    g_state_slot = 0;

    fd = sceIoOpen(SLOT_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;

    memset(buf, 0, sizeof(buf));
    read = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (read > 0) {
        int slot = atoi(buf);
        if (slot >= 0 && slot <= 9)
            g_state_slot = slot;
    }
}

int VitaSaveStates_GetSlot(void)
{
    return g_state_slot;
}

void VitaSaveStates_SetSlot(int slot)
{
    char buf[8];
    int fd;
    int len;

    while (slot < 0)
        slot += 10;
    slot %= 10;
    if (slot == g_state_slot)
        return;

    g_state_slot = slot;
    len = snprintf(buf, sizeof(buf), "%d\n", slot);
    fd = sceIoOpen(SLOT_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

bool VitaSaveStates_Exists(const char *game_path, int slot)
{
    char path[256];
    SceIoStat stat;

    make_state_path(game_path, slot, path, sizeof(path));
    memset(&stat, 0, sizeof(stat));
    if (sceIoGetstat(path, &stat) < 0)
        return false;
    return stat.st_size >= (SceOff)sizeof(VitaStateHeader);
}

int VitaSaveStates_Save(const char *game_path, int slot)
{
    char path[256];
    char tmp_path[264];
    const size_t state_size = retro_serialize_size();
    VitaStateHeader header;
    SceUID uid = -1;
    void *buffer = NULL;
    int fd = -1;
    int result = -1;

    if (alloc_state_buffer(state_size, &uid, &buffer) < 0)
        return -1;

    if (!retro_serialize(buffer, state_size))
        goto cleanup;

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, STATE_MAGIC, sizeof(header.magic));
    header.version = STATE_VERSION;
    header.data_size = (uint32_t)state_size;

    make_state_path(game_path, slot, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    fd = sceIoOpen(tmp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        goto cleanup;

    if (write_all(fd, &header, sizeof(header)) < 0 ||
        write_all(fd, buffer, state_size) < 0)
        goto cleanup;

    sceIoClose(fd);
    fd = -1;
    sceIoRemove(path);
    if (sceIoRename(tmp_path, path) < 0)
        goto cleanup;

    result = 0;

cleanup:
    if (fd >= 0)
        sceIoClose(fd);
    if (result < 0)
        sceIoRemove(tmp_path);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
    return result;
}

int VitaSaveStates_Load(const char *game_path, int slot)
{
    char path[256];
    const size_t state_size = retro_serialize_size();
    VitaStateHeader header;
    SceUID uid = -1;
    void *buffer = NULL;
    int fd = -1;
    int result = -1;

    make_state_path(game_path, slot, path, sizeof(path));
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;

    memset(&header, 0, sizeof(header));
    if (read_all(fd, &header, sizeof(header)) < 0)
        goto cleanup;
    if (memcmp(header.magic, STATE_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != STATE_VERSION ||
        header.data_size != state_size)
        goto cleanup;

    if (alloc_state_buffer(state_size, &uid, &buffer) < 0)
        goto cleanup;
    if (read_all(fd, buffer, state_size) < 0)
        goto cleanup;

    sceIoClose(fd);
    fd = -1;

    if (!retro_unserialize(buffer, state_size))
        goto cleanup;

    result = 0;

cleanup:
    if (fd >= 0)
        sceIoClose(fd);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
    return result;
}
