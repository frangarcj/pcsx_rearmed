/* Synthetic bus/CPU vectors for the Twinkle sound-board foundation.
 * These are not an authentic game boot or ATA/audio validation. */
#include "twinkle_board.h"
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static void put16(std::vector<uint8_t> &rom, size_t p, uint16_t v)
{ rom[p]=uint8_t(v>>8);rom[p+1]=uint8_t(v); }
static void put32(std::vector<uint8_t> &rom, size_t p, uint32_t v)
{ put16(rom,p,uint16_t(v>>16));put16(rom,p+2,uint16_t(v)); }

struct Fixture {
    std::vector<uint8_t> rom = std::vector<uint8_t>(TwinkleBoard::ROM_BYTES);
    std::vector<uint16_t> wave = std::vector<uint16_t>(TwinkleBoard::WAVE_RAM_BYTES/2,0);
    std::vector<std::array<int32_t,4>> audio;
    TwinkleBoard *board = nullptr;
    bool probe_reentry=false, reentry_rejected=false;
    static void sample(void *p,const int32_t *o,uint64_t)
    {
        auto &f=*static_cast<Fixture*>(p);
        f.audio.push_back({o[0],o[1],o[2],o[3]});
        if(f.probe_reentry) {
            f.reentry_rejected=f.board->reset()==TwinkleBoard::REENTRANT;
            f.probe_reentry=false;
        }
    }
    explicit Fixture(unsigned mode=0,const ata_storage_config_t *hdd=nullptr)
    {
        if (mode == 0) {
            put32(rom,0,0x0013fffc); // initial A7
            put32(rom,4,0x00000008); // initial PC
            size_t p=8;
            // MOVE.W #$5aa5,$00100000
            put16(rom,p,0x33fc);put16(rom,p+2,0x5aa5);put32(rom,p+4,0x00100000);p+=8;
            // MOVE.W #1,$00260000 selects 8 MiB wave-RAM bank 1
            put16(rom,p,0x33fc);put16(rom,p+2,0x0001);put32(rom,p+4,0x00260000);p+=8;
            // MOVE.W #$a55a,$00800000 writes bank-1's first sample word
            put16(rom,p,0x33fc);put16(rom,p+2,0xa55a);put32(rom,p+4,0x00800000);p+=8;
            // STOP #$2700
            put16(rom,p,0x4e72);put16(rom,p+2,0x2700);
        } else if (mode == 1) {
            put32(rom,0,0x0013fffc);put32(rom,4,8);
            put16(rom,8,0x60fe); // BRA to itself; let the chip clocks advance
        } else if (mode == 2) {
            put32(rom,0,0x0013fffc);put32(rom,4,8);put32(rom,0x70,0x200);
            put16(rom,8,0x46fc);put16(rom,10,0x2000); // enable autovector IRQs
            put16(rom,12,0x60fe); // wait loop
            put16(rom,0x200,0x33fc);put16(rom,0x202,0xbabe);
            put32(rom,0x204,0x00100000);put16(rom,0x208,0x4e73); // RTE
        } else {
            put32(rom,0,0x0013fffc);put32(rom,4,8);put32(rom,0x64,0x200);
            put16(rom,8,0x46fc);put16(rom,10,0x2000); // enable autovector IRQs
            put16(rom,12,0x60fe); // wait loop
            put16(rom,0x200,0x33fc);put16(rom,0x202,0xcafe);
            put32(rom,0x204,0x00100002);put16(rom,0x208,0x4e73); // RTE
        }
        TwinkleBoard::Config c{};c.program_rom=rom.data();c.program_rom_size=rom.size();
        c.wave_ram=wave.data();c.wave_ram_bytes=wave.size()*sizeof(uint16_t);
        c.rf5c400.sample=sample;c.rf5c400.sample_opaque=this;
        c.hdd=hdd;
        board=new TwinkleBoard(c);assert(board->valid());
    }
    ~Fixture(){delete board;}
};

static void cpu_and_wave_map()
{
    Fixture f;
    assert(f.board->run(200)>=0);
    assert(f.board->cpu_cycles()>=100);
    assert(f.board->cpu_pc()>=0x20);
    uint16_t v=0;
    assert(f.board->read16(0x100000,0xffff,&v)==TwinkleBoard::OK&&v==0x5aa5);
    assert(f.board->wave_bank()==1);
    assert(f.board->read16(0x800000,0xffff,&v)==TwinkleBoard::OK&&v==0xa55a);
    assert(f.wave[0x400000]==0xa55a);
    /* Hardware window remains 8 MiB; an invalid bank is not wrapped. */
    assert(f.board->write16(0x260000,8,0xffff)==TwinkleBoard::OK);
    assert(f.board->read16(0x800000,0xffff,&v)==TwinkleBoard::UNMAPPED);
}

static void masks_registers_and_mailbox()
{
    Fixture f;
    assert(f.board->write16(0x100000,0x1234,0xffff)==TwinkleBoard::OK);
    assert(f.board->write16(0x100000,0xab00,0xff00)==TwinkleBoard::OK);
    uint16_t v=0;
    assert(f.board->read16(0x100000,0xffff,&v)==TwinkleBoard::OK&&v==0xab34);
    assert(f.board->write16(0x400000,0x4567,0xffff)==TwinkleBoard::OK);
    assert(f.board->read16(0x400000,0xffff,&v)==TwinkleBoard::OK&&v==0x4567);
    assert(f.board->write16(0x260000,2,0xffff)==TwinkleBoard::OK);
    assert(f.board->write16(0x800000,0x1357,0xffff)==TwinkleBoard::OK);
    assert(f.wave[0x800000]==0x1357);
    assert(f.board->write16(0x300000,0,0xffff)==TwinkleBoard::UNSUPPORTED);
    assert(f.board->write16(0x220000,0x00f0,0xffff)==TwinkleBoard::OK&&f.board->led_outputs()==0x0f);
    assert(f.board->write16(0x240000,0x5678,0xffff)==TwinkleBoard::OK);
    assert(f.board->write16(0x250000,0x1234,0xffff)==TwinkleBoard::OK);
    assert(f.board->ata_dma_word_index()==0x12345678);
    assert(f.board->read16(0x34000e,0xffff,&v)==TwinkleBoard::OK&&v==0);
    for(uint32_t address:{0x240000u,0x240002u,0x250000u,0x250002u,0x260000u})
        assert(f.board->read16(address,0xffff,&v)==TwinkleBoard::OK&&v==0);
    assert(f.board->write16(0x34000e,0x1234,0xffff)==TwinkleBoard::OK);

    uint8_t b=0;
    assert(f.board->main_dpram_write(0x3fe,0x91)==TwinkleBoard::OK);
    assert(f.board->read16(0x2807fc,0xffff,&v)==TwinkleBoard::OK&&v==0x0091);
    assert(f.board->write16(0x2807fc,0x1200,0xff00)==TwinkleBoard::OK);
    assert(f.board->read16(0x2807fc,0xffff,&v)==TwinkleBoard::OK&&v==0x0091);
    assert(f.board->write16(0x2807fc,0x0034,0x00ff)==TwinkleBoard::OK);
    assert(f.board->read16(0x2807fc,0xffff,&v)==TwinkleBoard::OK&&v==0x0034);
    assert(f.board->main_dpram_read(0x3fe,&b)==TwinkleBoard::OK&&b==0x34);
    /* Sound-side read acknowledges CY7C131 IRQ4; no line is exposed to the
       guest-visible API, but the 68K's corresponding control clear is safe. */
    assert(f.board->write16(0x230000,0x0200,0xffff)==TwinkleBoard::OK);
    assert(f.board->write16(0x230000,0x0000,0xffff)==TwinkleBoard::OK);
}

static void irq_and_audio_clock()
{
    Fixture irq(2);
    assert(irq.board->main_dpram_write(0x3fe,0x5a)==TwinkleBoard::OK);
    uint16_t value=0;
    /* Only the mapped low byte lane invokes the CY7C131 read acknowledge. */
    assert(irq.board->read16(0x2807fc,0xff00,&value)==TwinkleBoard::OK&&value==0);
    assert(irq.board->run(1000)>=0);
    assert(irq.board->read16(0x100000,0xffff,&value)==TwinkleBoard::OK&&value==0xbabe);
    assert(irq.board->read16(0x2807fc,0xffff,&value)==TwinkleBoard::OK&&value==0x005a);

    Fixture acked(2);
    assert(acked.board->main_dpram_write(0x3fe,0x5a)==TwinkleBoard::OK);
    assert(acked.board->read16(0x2807fc,0x00ff,&value)==TwinkleBoard::OK&&value==0x005a);
    assert(acked.board->run(1000)>=0);
    assert(acked.board->read16(0x100000,0xffff,&value)==TwinkleBoard::OK&&value==0);

    /* The 60 Hz IRQ is asserted at source cycle floor(n*16MHz/60), not at
       the end of an arbitrarily large run slice. Long/split budgets agree. */
    Fixture timer_long(3), timer_split(3);
    assert(timer_long.board->run(266766)>=0);
    assert(timer_split.board->run(133333)>=0);
    assert(timer_split.board->run(133433)>=0);
    assert(timer_long.board->run(100)>=0);
    assert(timer_split.board->run(100)>=0);
    assert(timer_long.board->read16(0x100002,0xffff,&value)==TwinkleBoard::OK&&value==0xcafe);
    uint16_t other=0;
    assert(timer_split.board->read16(0x100002,0xffff,&other)==TwinkleBoard::OK&&other==0xcafe);
    assert(timer_long.board->cpu_cycles()==timer_split.board->cpu_cycles());
    assert(timer_long.board->write16(0x230000,0x0080,0xffff)==TwinkleBoard::OK);
    assert(timer_long.board->write16(0x230000,0x0000,0xffff)==TwinkleBoard::OK);
    assert(timer_long.board->write16(0x260000,2,0xffff)==TwinkleBoard::OK);
    assert(timer_long.board->write16(0x230000,0x0077,0xffff)==TwinkleBoard::OK);
    const uint64_t rf_before_reset=timer_long.board->rf_cycles();
    const uint64_t before_reset=timer_long.board->cpu_cycles();
    assert(timer_long.board->reset()==TwinkleBoard::OK);
    assert(timer_long.board->cpu_cycles()==before_reset);
    assert(timer_long.board->wave_bank()==2&&timer_long.board->control()==0x0077);
    assert(timer_long.board->write16(0x100002,0,0xffff)==TwinkleBoard::OK);
    const uint32_t until_next_irq=uint32_t(533333-before_reset);
    assert(timer_long.board->run(until_next_irq)>=0);
    const int64_t after_reset_run=timer_long.board->run(100);
    if(after_reset_run<0) std::fprintf(stderr,"second timer run=%lld cpu=%llu rf=%llu pc=%x sr=%x\n",
        (long long)after_reset_run,(unsigned long long)timer_long.board->cpu_cycles(),
        (unsigned long long)timer_long.board->rf_cycles(),timer_long.board->cpu_pc(),timer_long.board->cpu_sr());
    assert(after_reset_run>=0);
    assert(timer_long.board->read16(0x100002,0xffff,&value)==TwinkleBoard::OK&&value==0xcafe);
    assert(timer_long.board->rf_cycles()>=rf_before_reset);

    Fixture audio(1);
    audio.wave[0]=0x4000;
    auto w=[&](unsigned r,uint16_t x){assert(audio.board->write16(0x400000+r*2,x,0xffff)==TwinkleBoard::OK);};
    w(0x400,0);w(0x401,0);w(0x402,0x5000);w(0x403,127);w(0x404,0);
    w(0x405,128);w(0x406,0);w(0x407,0);w(0x408,0);w(0x409,0);
    w(0x40c,0x80);w(0x40e,0);w(1,0x60);
    audio.probe_reentry=true;
    assert(audio.board->run(5000)>=0);
    assert(!audio.audio.empty());
    assert(audio.reentry_rejected&&audio.board->callback_fault());
    bool nonzero=false;for(const auto &frame:audio.audio)for(int32_t s:frame)nonzero|=s!=0;
    assert(nonzero);
}

static void validation()
{
    std::vector<uint16_t> wave(TwinkleBoard::WAVE_RAM_BYTES/2,0);
    std::vector<uint8_t> rom(TwinkleBoard::ROM_BYTES);
    TwinkleBoard::Config c{};c.program_rom=rom.data();c.program_rom_size=rom.size()-1;
    c.wave_ram=wave.data();c.wave_ram_bytes=wave.size()*sizeof(wave[0]);
    TwinkleBoard b(c);assert(!b.valid()&&b.error()==TwinkleBoard::ARGUMENT);
}

struct DiskImage {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(16*512);
    unsigned reads=0;
    bool fail=false;
    static int read(void *opaque,uint32_t lba,void *out) {
        auto &d=*static_cast<DiskImage *>(opaque);d.reads++;
        if(d.fail || lba>=16)return 0;
        std::memcpy(out,d.bytes.data()+lba*512,512);return 1;
    }
    static int write(void *,uint32_t,const void *) { return 0; } // Explicit read-only fixture.
    ata_storage_config_t config() {
        ata_storage_config_t c{};c.sector_count=16;c.cylinders=1;c.heads=1;c.sectors_per_track=16;
        c.media_opaque=this;c.read_sector=read;c.write_sector=write;c.dma_transfer_time_ns=750000;return c;
    }
    DiskImage(){for(unsigned i=0;i<bytes.size();i++)bytes[i]=uint8_t(i*17+3);}
};

static void ata_register(Fixture &f,unsigned reg,uint16_t value)
{assert(f.board->write16(0x300000+reg*2,value,0xffff)==TwinkleBoard::OK);}
static uint16_t ata_read(Fixture &f,unsigned reg)
{uint16_t value;assert(f.board->read16(0x300000+reg*2,0xffff,&value)==TwinkleBoard::OK);return value;}
static void ata_lba(Fixture &f,unsigned count,unsigned lba)
{ata_register(f,2,count);ata_register(f,3,lba);ata_register(f,4,0);ata_register(f,5,0);ata_register(f,6,0x40);}

static void ata_pio_and_error()
{
    DiskImage d;auto cfg=d.config();Fixture f(1,&cfg);
    assert(f.board->ata_status()&0x80);assert(f.board->run(64000)==64000);
    assert(!(f.board->ata_status()&0x80));
    uint16_t dip;assert(f.board->read16(0x200000,0xffff,&dip)==TwinkleBoard::OK && dip==0xff);
    ata_register(f,7,0xec);assert(f.board->run(160)==160);
    assert(ata_read(f,0)==0x045a); // Source IDE HDD identity, not CompactFlash.
    for(unsigned i=1;i<256;i++)(void)ata_read(f,0);
    ata_lba(f,1,3);ata_register(f,7,0x20);assert(f.board->run(200)==200);
    assert(f.board->ata_status()&8);
    for(unsigned i=0;i<256;i++)assert(ata_read(f,0)==uint16_t(d.bytes[3*512+i*2]|(d.bytes[3*512+i*2+1]<<8)));
    assert(d.reads==1 && f.board->ata_dma_words()==0);
    d.fail=true;ata_lba(f,1,4);ata_register(f,7,0x20);assert(f.board->run(200)==200);
    assert(f.board->ata_status()&1);assert(f.board->ata_dma_words()==0 && f.board->valid());
}

static void ata_dma_cpu_stop_and_slice_equivalence()
{
    std::vector<uint16_t> expected;
    for(unsigned quantum:{100000u,17u}) {
        DiskImage d;auto cfg=d.config();Fixture f(1,&cfg);assert(f.board->run(64000)==64000);
        // Configure DMA mode through native CS0, then execute READ DMA/STOP
        // on the actual MC68000. IRQ6 must wake it before the 60 Hz IRQ1.
        ata_register(f,1,3);ata_register(f,2,0x22);ata_register(f,7,0xef);assert(f.board->run(200)==200);
        assert(f.board->write16(0x260000,1,0xffff)==TwinkleBoard::OK);
        assert(f.board->write16(0x240000,0x10,0xffff)==TwinkleBoard::OK);
        assert(f.board->write16(0x250000,0,0xffff)==TwinkleBoard::OK);
        ata_lba(f,2,1);put32(f.rom,0x78,0x200);size_t p=8;
        auto store=[&](uint16_t value,uint32_t address){put16(f.rom,p,0x33fc);put16(f.rom,p+2,value);put32(f.rom,p+4,address);p+=8;};
        store(0x0400,0x230000);store(0xc8,0x30000e);
        put16(f.rom,p,0x4e72);put16(f.rom,p+2,0x2000);put16(f.rom,p+4,0x60fe);
        p=0x200;store(0xcafe,0x100000);store(0,0x230000);put16(f.rom,p,0x4e73);
        assert(f.board->reset()==TwinkleBoard::OK);
        uint64_t start=f.board->cpu_cycles();
        for(unsigned done=0;done<100000;){unsigned n=std::min(quantum,100000-done);assert(f.board->run(n)==n);done+=n;}
        assert(f.board->cpu_cycles()==start+100000);
        assert(f.board->ata_dma_words()==512 && f.board->ata_dma_word_index()==0x210 && d.reads==2);
        uint16_t marker;assert(f.board->read16(0x100000,0xffff,&marker)==TwinkleBoard::OK && marker==0xcafe);
        for(unsigned i=0;i<512;i++)assert(f.wave[0x400010+i]==uint16_t(d.bytes[512+i*2]|(d.bytes[512+i*2+1]<<8)));
        assert(!f.board->callback_fault() && f.board->valid());
        if(expected.empty())expected=f.wave;else assert(expected==f.wave);
        const auto before=f.board->ata_dma_words();assert(f.board->reset()==TwinkleBoard::OK);
        assert(f.board->ata_dma_words()==before && f.wave[0x400010]==expected[0x400010]);
    }
}

static void ata_dma_bounds()
{
    DiskImage d;auto cfg=d.config();Fixture f(1,&cfg);assert(f.board->run(64000)==64000);
    ata_register(f,1,3);ata_register(f,2,0x22);ata_register(f,7,0xef);assert(f.board->run(200)==200);
    assert(f.board->write16(0x260000,3,0xffff)==TwinkleBoard::OK);
    assert(f.board->write16(0x240000,0xffff,0xffff)==TwinkleBoard::OK);
    assert(f.board->write16(0x250000,0x3f,0xffff)==TwinkleBoard::OK);
    ata_lba(f,1,1);ata_register(f,7,0xc8);
    assert(f.board->run(200)<0 && !f.board->valid() && f.board->error()==TwinkleBoard::DEVICE_ERROR);
    assert(f.board->ata_dma_words()==1 && f.board->ata_dma_word_index()==0x400000);
    assert(f.wave.back()==uint16_t(d.bytes[512]|(d.bytes[513]<<8)) && f.wave.front()==0);
}

int main()
{
    cpu_and_wave_map();masks_registers_and_mailbox();irq_and_audio_clock();validation();
    ata_pio_and_error();ata_dma_cpu_stop_and_slice_equivalence();ata_dma_bounds();
    puts("Twinkle MC68000/RF5C400/IDE HDD PIO and DMA, STOP/IRQ6 scheduling and split-run vectors PASS (synthetic, not original gameplay)");
}
