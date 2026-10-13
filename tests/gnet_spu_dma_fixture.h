/* GPL-2.0-or-later. MAME-derived G-Net SPU DMA acceptance vectors. */
#include "../libpcsxcore/psxevents.h"
#include "../libpcsxcore/psxhw.h"

static int harness_gnet_spu_dma_fixture(void)
{
  /* COH3002T uses 50 MHz PSX cycles. The source's 24000/33868800 s
   * DMA delay reaches cycle 35431; the payload is delivered at that edge. */
  const u32 start = psxRegs.cycle;
  const u32 source_delay = 35431;
  int16_t before[8] = {0}, after[8] = {0};
  int failed = PSXCLK != 50000000;
  psxMemWrite32(&psxRegs, 0x80004000, 0x56781234);
  psxMemWrite32(&psxRegs, 0x80004004, 0xdef09abc);
  psxMemWrite32(&psxRegs, 0x80004008, 0x24681357);
  psxMemWrite32(&psxRegs, 0x8000400c, 0xbbbbaaaa);
  SPU_writeRegister(0x1f801da6, 0, start);
  psxHwWriteDmaPcr32(0x00080000);
  psxHwWriteDmaIcr32(0x00900000);
  psxMemWrite32(&psxRegs, 0x1f8010c0, 0x80004000);
  psxMemWrite32(&psxRegs, 0x1f8010c4, 0x00010004);
  psxHwWriteChcr4(0x01000201);
  SPU_readDMAMem((unsigned short *)before, 8, start);
  for (unsigned i = 0; i < 8; i++) if (before[i]) failed = 1;
  psxRegs.cycle = start + source_delay - 1;
  irq_test(&psxRegs.CP0);
  if (!(psxHu32(0x10c8) & 0x01000000)) failed = 1;
  SPU_writeRegister(0x1f801da6, 0, psxRegs.cycle);
  psxRegs.cycle++;
  irq_test(&psxRegs.CP0);
  if ((psxHu32(0x10c8) & 0x01000000) || psxHu32(0x10c0) != 0x80004000 ||
      !(psxHu32(0x10f4) & 0x10000000) || !(psxHu16(0x1070) & 8)) failed = 1;
  SPU_writeRegister(0x1f801da6, 0, psxRegs.cycle);
  SPU_readDMAMem((unsigned short *)after, 8, psxRegs.cycle);
  if (memcmp(after, psxRegs.ptrs.psxM + 0x4000, sizeof(after))) failed = 1;
  /* Read DMA is immediate in the source, with RAM masking at the edge. */
  SPU_writeRegister(0x1f801da6, 0, psxRegs.cycle);
  psxMemWrite32(&psxRegs, 0x1f8010c0, 0x803ffffc);
  psxMemWrite32(&psxRegs, 0x1f8010c4, 0x00010002);
  psxHwWriteChcr4(0x01000200);
  if (psxMemRead32(&psxRegs, 0x803ffffc) != 0x56781234 ||
      psxMemRead32(&psxRegs, 0x80000000) != 0xdef09abc ||
      psxHu32(0x10c0) != 0x803ffffc || (psxHu32(0x10c8) & 0x01000000)) failed = 1;
  fprintf(stderr, "SystemGNet SPU DMA fixture: %s delayed_payload=1 ticks=24000 boundary_cycle=35431 madr_retained=1 read_wrap=1\n", failed ? "FAIL" : "PASS");
  return failed;
}
