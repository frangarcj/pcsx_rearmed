/* JVS cabinet endpoint. Packet framing and command semantics follow MAME
 * jvshle.cpp (smf, BSD-3-Clause), deb2f54f. Integration GPL-2.0-or-later.
 * Replies are delivered as 115200-baud 8N1 pin edges by the board scheduler. */
#ifndef PCSX_SYSTEM12_JVS_H
#define PCSX_SYSTEM12_JVS_H
#include <stdint.h>
#include <string.h>
typedef struct {
    uint8_t address, rx[258], tx[520], escape, receiving, prev_coin[2], allow_address;
    unsigned rx_size, tx_size, tx_index, tx_bit;
    uint16_t coins[2];
    uint32_t outputs, packets, errors;
    uint64_t next_bit;
} S12Jvs;

static void s12_jvs_reset(S12Jvs *j)
{
    memset(j, 0, sizeof(*j)); j->address = 0xff; j->allow_address = 1;
}

static void s12_jvs_coin(S12Jvs *j, unsigned slot, unsigned pressed)
{
    if (slot < 2) {
        if (pressed && !j->prev_coin[slot]) j->coins[slot] = (j->coins[slot] + 1) & 0x3fff;
        j->prev_coin[slot] = pressed != 0;
    }
}

static void s12_jvs_encode(S12Jvs *j, const uint8_t *data, unsigned length)
{
    unsigned i;
    j->tx_size = 0; j->tx_index = j->tx_bit = 0;
    j->tx[j->tx_size++] = 0xe0;
    for (i = 0; i < length; i++) {
        uint8_t b = data[i];
        if (b == 0xd0 || b == 0xe0) { j->tx[j->tx_size++] = 0xd0; b--; }
        j->tx[j->tx_size++] = b;
    }
}

static void s12_jvs_packet(S12Jvs *j, const uint32_t player[2], uint32_t system,
    const uint16_t analog[8], int techno, uint64_t now)
{
    uint8_t response[520] = {0, 0, 1}, checksum = 0;
    unsigned i, at = 2, used = 3, end = j->rx_size - 1;
    if (j->rx[0] != 0xff && j->rx[0] != j->address) return;
    for (i = 0; i < end; i++) checksum += j->rx[i];
    if (checksum != j->rx[end]) { j->errors++; return; }
    j->packets++;
    while (at < end && used < 240) {
        uint8_t command = j->rx[at++];
        unsigned n, k;
#define JVS_NEED(count) do { if (end - at < (count)) { response[used++] = 2; at = end; goto command_done; } } while (0)
        switch (command) {
        case 0xf0:
            JVS_NEED(1);
            if (j->rx[at++] == 0xd9) { j->address = 0xff; j->coins[0] = j->coins[1] = 0; }
            break;
        case 0xf1:
            JVS_NEED(1);
            if (j->allow_address && j->address == 0xff) { j->address = j->rx[at]; response[used++] = 1; }
            at++; break;
        case 0x10: {
            const char *id = techno == 2 ? "namco ltd.;EM Pri1-01;Ver2.00;JPN&EXP,Techno-Drive PRN" :
                techno == 1 ? "namco ltd.;EM I/O1-02;Ver2.00;JPN&EXP,Techno-Drive I/O" :
                "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551;Ver1.00";
            response[used++] = 1; n = strlen(id) + 1;
            memcpy(response + used, id, n); used += n; break;
        }
        case 0x11: response[used++] = 1; response[used++] = techno == 1 ? 0x11 : 0x13; break;
        case 0x12: response[used++] = 1; response[used++] = techno == 1 ? 0x20 : 0x30; break;
        case 0x13: response[used++] = 1; response[used++] = 0x10; break;
        case 0x14: {
            const uint8_t features[] = {1, 1, 2, 16, 0, 2, 2, 0, 0, 3, 8, 16, 0, 0x12, 32, 0, 0, 0};
            const uint8_t emio_features[] = {1, 1, 1, 19, 0, 2, 2, 0, 0, 3, 8, 0, 0, 0x12, 24, 0, 0, 0};
            if (techno == 2) { response[used++] = 1; response[used++] = 0; }
            else if (techno == 1) { memcpy(response + used, emio_features, sizeof(emio_features)); used += sizeof(emio_features); }
            else { memcpy(response + used, features, sizeof(features)); used += sizeof(features); }
            break;
        }
        case 0x15:
            while (at < end && j->rx[at++]) {}
            response[used++] = 1; break;
        case 0x20:
            JVS_NEED(2); n = j->rx[at++]; k = j->rx[at++];
            if (n > (techno == 1 ? 1 : 2) || k > (techno == 1 ? 3 : 2)) { response[used++] = 3; break; }
            response[used++] = 1; response[used++] = system & 0xff;
            for (i = 0; i < n; i++) {
                unsigned b;
                for (b = 0; b < k; b++) response[used++] = player[i] >> (b * 8);
            }
            break;
        case 0x21:
            JVS_NEED(1); n = j->rx[at++];
            if (n > 2) { response[used++] = 3; break; }
            response[used++] = 1;
            for (i = 0; i < n; i++) { response[used++] = j->coins[i] >> 8; response[used++] = j->coins[i]; }
            break;
        case 0x22:
            JVS_NEED(1); n = j->rx[at++];
            if (n > 8) { response[used++] = 3; break; }
            response[used++] = 1;
            for (i = 0; i < n; i++) { response[used++] = analog[i] >> 8; response[used++] = analog[i]; }
            break;
        case 0x30: case 0x35:
            JVS_NEED(3); n = j->rx[at++]; k = j->rx[at] * 256 + j->rx[at + 1]; at += 2;
            response[used++] = n >= 1 && n <= 2 ? 1 : 3;
            if (n >= 1 && n <= 2) {
                if (command == 0x30) j->coins[n - 1] = j->coins[n - 1] < k ? 0 : j->coins[n - 1] - k;
                else j->coins[n - 1] = (j->coins[n - 1] + k) & 0x3fff;
            }
            break;
        case 0x32:
            JVS_NEED(1); n = j->rx[at++]; JVS_NEED(n);
            if (n > 4) { at += n; response[used++] = 3; break; }
            j->outputs = 0;
            for (i = 0; i < n; i++) j->outputs = (j->outputs << 8) | j->rx[at++];
            response[used++] = 1; break;
        case 0x72:
            JVS_NEED(1); n = j->rx[at++];
            if (techno != 2) { response[2] = 2; used = 3; at = end; break; }
            if (n == 1 || n == 2 || n == 4) response[used++] = 1;
            else if (n == 3) { response[used++] = 1; response[used++] = 1; }
            else if (n == 5) { JVS_NEED(1); response[used++] = j->rx[at++] == 1 ? 1 : 3; }
            else if (n == 7) { JVS_NEED(1); k = j->rx[at++]; JVS_NEED(k); at += k; response[used++] = 1; }
            else { response[2] = 2; used = 3; at = end; }
            break;
        case 0x70:
            JVS_NEED(1); n = j->rx[at++];
            if (techno == 1 && n == 4) { response[used++] = 1; response[used++] = 0xff; response[used++] = 0xff; }
            else if (techno == 1 && n == 2) {
                /* Firmware build date returned by th1io-a.4f in the MAME serial oracle. */
                const uint8_t version[] = {1, 0x19, 0x98, 4, 9, 4, 0x15, 0x11, 0x14};
                memcpy(response + used, version, sizeof(version)); used += sizeof(version);
            } else { response[2] = 2; used = 3; at = end; }
            break;
        default: response[2] = 2; used = 3; at = end; break;
        }
command_done:
#undef JVS_NEED
        if (used > 256) { response[2] = 4; used = 3; break; }
    }
    if (used == 3 && response[2] == 1) return;
    response[1] = used - 1; checksum = 0;
    for (i = 0; i < used; i++) checksum += response[i];
    response[used++] = checksum;
    s12_jvs_encode(j, response, used);
    j->next_bit = now + 1470; /* MAME's one-character response delay. */
}

static void s12_jvs_receive(S12Jvs *j, uint8_t byte, const uint32_t player[2],
    uint32_t system, const uint16_t analog[8], int techno, uint64_t now)
{
    if (byte == 0xe0) { j->rx_size = 0; j->escape = 0; j->receiving = 1; return; }
    if (!j->receiving) return;
    if (byte == 0xd0 && !j->escape) { j->escape = 1; return; }
    if (j->escape) { byte++; j->escape = 0; }
    if (j->rx_size >= sizeof(j->rx)) { j->errors++; j->receiving = 0; return; }
    j->rx[j->rx_size++] = byte;
    if (j->rx_size >= 2 && j->rx_size == (unsigned)j->rx[1] + 2) {
        if (j->rx_size >= 4) s12_jvs_packet(j, player, system, analog, techno, now);
        j->receiving = 0;
    }
}

static int s12_jvs_tx_bit(S12Jvs *j)
{
    int level = j->tx_bit == 0 ? 0 : j->tx_bit == 9 ? 1 :
        (j->tx[j->tx_index] >> (j->tx_bit - 1)) & 1;
    if (++j->tx_bit == 10) { j->tx_bit = 0; j->tx_index++; }
    j->next_bit += 147;
    return level;
}
#endif
