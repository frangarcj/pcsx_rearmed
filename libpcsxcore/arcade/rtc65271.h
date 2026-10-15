/* Source-derived Epson RTC65271, BSD-3-Clause; see rtc65271_LICENSE.txt. */
#ifndef PCSX_RTC65271_H
#define PCSX_RTC65271_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Rtc65271 Rtc65271;
enum { RTC65271_NVRAM_BYTES=4152, RTC65271_OK=0, RTC65271_ARGUMENT=-1,
       RTC65271_BUSY=-2, RTC65271_FAULT=-3 };
typedef struct Rtc65271Date {
    int year,month,day,weekday,hour,minute,second; /* weekday: Sunday=1 */
} Rtc65271Date;
typedef struct Rtc65271Config {
    Rtc65271Date date; /* Explicit framework-equivalent initial host time. */
    const uint8_t *nvram;size_t nvram_size; /* Optional original source format. */
    void (*irq)(void *,int);void *opaque;
} Rtc65271Config;
typedef struct Rtc65271Status {
    uint64_t now_ns,begin_updates,end_updates,square_edges,irq_callbacks;
    uint8_t regs[64],register_index,xram_page,square_state,irq;
    unsigned faults;
} Rtc65271Status;
int rtc65271_create(const Rtc65271Config *,Rtc65271 **);
int rtc65271_try_destroy(Rtc65271 *);
/* Single-owner API. Monotonic absolute nanoseconds; internal fractional periodic phases are
 * retained at attosecond precision. Callback mutation/destruction is refused. */
int rtc65271_advance_to(Rtc65271 *,uint64_t ns);
int rtc65271_read(Rtc65271 *,int xram,unsigned offset,uint8_t *);
int rtc65271_write(Rtc65271 *,int xram,unsigned offset,uint8_t);
/* Source NVRAM: version/control/alarm/user bytes and all 4096 XRAM bytes.
 * The clock, indirect address/page and timer phase are not machine save state. */
int rtc65271_nvram_export(const Rtc65271 *,uint8_t *,size_t);
void rtc65271_get_status(const Rtc65271 *,Rtc65271Status *);
#ifdef __cplusplus
}
#endif
#endif
