#include "../libpcsxcore/arcade/system12_jvs.h"
#include <assert.h>
#include <stdio.h>

static void send_mode(S12Jvs *j, uint8_t address, const uint8_t *payload,
    unsigned length, const uint32_t players[2], const uint16_t analog[8], int mode)
{
    uint8_t packet[258], sum = address + length + 1;
    unsigned i;
    packet[0] = address; packet[1] = length + 1;
    memcpy(packet + 2, payload, length);
    for (i = 0; i < length; i++) sum += payload[i];
    packet[length + 2] = sum;
    S12Jvs encoder = {0};
    s12_jvs_encode(&encoder, packet, length + 3);
    for (i = 0; i < encoder.tx_size; i++)
        s12_jvs_receive(j, encoder.tx[i], players, 0x80, analog, mode, 100);
}
static void send_packet(S12Jvs *j, uint8_t address, const uint8_t *payload,
    unsigned length, const uint32_t players[2], const uint16_t analog[8]) {
    send_mode(j,address,payload,length,players,analog,0);
}

int main(void)
{
    S12Jvs j;
    const uint32_t players[2] = {0x8080, 0x0204};
    const uint16_t analog[8] = {0xd0e0, 0x5e00};
    const uint8_t address[] = {0xf1, 1};
    const uint8_t controls[] = {0x20, 2, 2, 0x21, 2, 0x22, 2};
    const uint8_t outputs[] = {0x32, 2, 0xd0, 0xe0};
    s12_jvs_reset(&j);
    send_packet(&j, 0xff, address, sizeof(address), players, analog);
    assert(j.address == 1 && j.packets == 1 && j.next_bit == 1570);
    assert(s12_jvs_tx_bit(&j) == 0); /* physical UART start bit */
    assert(s12_jvs_tx_bit(&j) == 0); /* sync byte e0, bit 0 */
    s12_jvs_coin(&j, 0, 1); s12_jvs_coin(&j, 0, 1);
    assert(j.coins[0] == 1);
    s12_jvs_coin(&j, 0, 0); s12_jvs_coin(&j, 0, 1);
    assert(j.coins[0] == 2);
    send_packet(&j, 1, controls, sizeof(controls), players, analog);
    assert(j.errors == 0 && j.packets == 2);
    /* Reply reports byte order used by System 12's H8 firmware. */
    assert(j.tx[0] == 0xe0 && j.tx[3] == 1 && j.tx[4] == 1);
    assert(j.tx[5] == 0x80 && j.tx[6] == 0x80 && j.tx[7] == 0x80);
    send_packet(&j, 1, outputs, sizeof(outputs), players, analog);
    assert(j.outputs == 0xd0e0); /* escaping does not alter the payload */
    unsigned packets = j.packets;
    send_packet(&j, 2, controls, sizeof(controls), players, analog);
    assert(j.packets == packets); /* wrong destination is ignored */
    uint8_t bad[] = {0xe0, 1, 2, 0x11, 0};
    for (unsigned i = 0; i < sizeof(bad); i++)
        s12_jvs_receive(&j, bad[i], players, 0, analog, 0, 200);
    assert(j.errors == 1 && j.packets == packets);
    S12Jvs printer, emio;
    s12_jvs_reset(&printer); s12_jvs_reset(&emio); emio.allow_address=0;
    send_mode(&printer,0xff,address,sizeof(address),players,analog,2);
    send_mode(&emio,0xff,address,sizeof(address),players,analog,1);
    assert(printer.address==1 && emio.address==0xff);
    emio.allow_address=1;
    const uint8_t second[]={0xf1,2}, capabilities[]={0x11,0x12,0x13,0x14};
    send_mode(&printer,0xff,second,sizeof(second),players,analog,2);
    send_mode(&emio,0xff,second,sizeof(second),players,analog,1);
    assert(printer.address==1 && emio.address==2);
    send_mode(&emio,2,capabilities,sizeof(capabilities),players,analog,1);
    /* Captured from th1io-a.4f through MAME's real H8 serial receive pins. */
    const uint8_t oracle[]={1,1,0x11,1,0x20,1,0x10,1,1,1,19,0,2,2,0,0,3,8,0,0,0x12,24,0,0,0};
    assert(!memcmp(emio.tx+3,oracle,sizeof(oracle)));
    const uint8_t status[]={0x72,3};
    send_mode(&printer,1,status,sizeof(status),players,analog,2);
    assert(printer.tx[3]==1 && printer.tx[4]==1 && printer.tx[5]==1);
    puts("System12 JVS framing, UART bits, address, coin edges, inputs and outputs PASS");
}
