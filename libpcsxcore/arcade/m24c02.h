/* BSD-3-Clause; smf. See m24c02_LICENSE.txt. */
#ifndef PCSX_M24C02_H
#define PCSX_M24C02_H
#include <array>
#include <cstdint>
/* Single-owner, pin-clocked source M24C02-W. The caller owns exactly 256
 * mutable NVRAM bytes for the lifetime of the device. No synthesized replies,
 * clock/timer or reset API: the source device has no device_reset override. */
class M24c02 {
public:
    explicit M24c02(std::array<uint8_t,256> &data):m_data(data) {}
    void write_sda(int);void write_scl(int);int read_sda();
    void write_e0(int);void write_e1(int);void write_e2(int);void write_wc(int);
private:
    int address_mask();int select_device();int data_offset();
    bool skip_addresshigh() const {return m_data_size<=0x800;}
    struct Machine {bool side_effects_disabled() const {return false;}};
    Machine machine() const {return {};}
    std::array<uint8_t,256> &m_data;
    int m_slave_address=0xa0,m_read_page_size=0,m_write_page_size=16,m_data_size=256;
    int m_scl=0,m_sdaw=0,m_e0=0,m_e1=0,m_e2=0,m_wc=0,m_sdar=1;
    int m_state=0,m_bits=0,m_shift=0,m_devsel=0,m_addresshigh=0,m_byteaddr=0;
    std::array<uint8_t,16> m_page{};
    int m_page_offset=0,m_page_written_size=0;
    bool m_devsel_address_low=false;
};
#endif
