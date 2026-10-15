/* BSD-3-Clause. See twinkle_serial_LICENSE.txt. */
#ifndef PCSX_TWINKLE_SERIAL_H
#define PCSX_TWINKLE_SERIAL_H
#include <cstdint>
#include <memory>
/* Source NS16550 pair + FDC37C665GT decoder/configuration + JVC XV-D701
 * serial endpoint. No video decoder. Floppy/parallel accesses fail explicitly.
 * Single-owner monotonic ns API. */
class TwinkleSerial {
public:
    struct Status {
        uint64_t now_ns,reads,writes,uart_tx_updates,uart_rx_updates,dvd_commands,dvd_response_bytes;
        unsigned faults;uint8_t uart_lcr[2],uart_lsr[2],uart_fcr[2];uint16_t uart_divisor[2];
        unsigned chapter,playback;bool powered;
    };
    TwinkleSerial();~TwinkleSerial();
    TwinkleSerial(const TwinkleSerial &)=delete;TwinkleSerial &operator=(const TwinkleSerial &)=delete;
    int advance_to(uint64_t ns);
    int read(unsigned offset,uint8_t *value);int write(unsigned offset,uint8_t value);
    void status(Status &) const;const char *error() const;
private:
    struct Impl;std::unique_ptr<Impl> p;
};
#endif
