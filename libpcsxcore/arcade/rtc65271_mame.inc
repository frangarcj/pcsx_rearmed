// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a. BSD-3-Clause.
// Raphael Nabet, R. Belmont; see rtc65271_LICENSE.txt.
// Framework timer declarations adapted; two month-table bounds guards.
enum
{
	reg_second = 0,
	reg_alarm_second,
	reg_minute,
	reg_alarm_minute,
	reg_hour,
	reg_alarm_hour,
	reg_weekday,
	reg_monthday,
	reg_month,
	reg_year,
	reg_A,
	reg_B,
	reg_C,
	reg_D
};

enum
{
	reg_A_UIP   = 0x80,
	reg_A_DV    = 0x70,
	reg_A_RS    = 0x0F,

	reg_B_SET   = 0x80,
	reg_B_PIE   = 0x40,
	reg_B_AIE   = 0x20,
	reg_B_UIE   = 0x10,
	reg_B_SQW   = 0x08,
	reg_B_DM    = 0x04,
	reg_B_24h   = 0x02,
	reg_B_DSE   = 0x01,

	reg_C_IRQF  = 0x80,
	reg_C_PF    = 0x40,
	reg_C_AF    = 0x20,
	reg_C_UF    = 0x10,

	reg_D_VRT   = 0x80
};

static const int SQW_freq_table[16] =
{
	0,
	256,
	128,
	8192,
	4096,
	2048,
	1024,
	512,
	256,
	128,
	64,
	32,
	16,
	8,
	4,
	2,
};


/*
    BCD utilities
*/

/*
    Increment a binary-encoded uint8_t
*/
static uint8_t increment_binary(uint8_t data)
{
	return data+1;
}


/*
    Increment a BCD-encoded uint8_t
*/
static uint8_t increment_BCD(uint8_t data)
{
	if ((data & 0x0f) < 0x09)
	{
		if ((data & 0xf0) < 0xa0)
			data++;
		else
			data = data + 0x01 - 0xa0;
	}
	else
	{
		if ((data & 0xf0) < 0xa0)
			data = data - 0x09 + 0x10;
		else
			data = data - 0x09 - 0x90;
	}
	return data;
}


/*
    Convert a binary-encoded uint8_t to BCD
*/
static uint8_t binary_to_BCD(uint8_t data)
{
	data %= 100;

	return ((data / 10) << 4) | (data %10);
}


/*
    Convert a BCD-encoded uint8_t to binary
*/
static uint8_t BCD_to_binary(uint8_t data)
{
	if ((data & 0x0f) >= 0x0a)
		data = data - 0x0a + 0x10;
	if ((data & 0xf0) >= 0xa0)
		data = data - 0xa0;

	return (data & 0x0f) + (((data & 0xf0) >> 4) * 10);
}



void rtc65271_device::rtc_clock_updated(int year, int month, int day, int day_of_week, int hour, int minute, int second)
{
	/* set clock registers */
	m_regs[reg_second] = second;
	m_regs[reg_minute] = minute;
	if (m_regs[reg_B] & reg_B_24h)
		/* 24-hour mode */
		m_regs[reg_hour] = hour;
	else
	{   /* 12-hour mode */
		if (hour >= 12)
		{
			m_regs[reg_hour] = 0x80;
			hour -= 12;
		}
		else
		{
			m_regs[reg_hour] = 0;
		}

		// Firebeat indicates non-BCD 12-hour mode has 0-based hour, so 12 AM is 0x00 and 12 PM is 0x80
		m_regs[reg_hour] |= hour; // ? hour : 12;
	}
	m_regs[reg_weekday] = day_of_week;
	m_regs[reg_monthday] = day;
	m_regs[reg_month] = month;
	m_regs[reg_year] = year % 100;
	if (! (m_regs[reg_B] & reg_B_DM))
	{   /* BCD mode */
		m_regs[reg_second] = binary_to_BCD(m_regs[reg_second]);
		m_regs[reg_minute] = binary_to_BCD(m_regs[reg_minute]);
		m_regs[reg_hour] = (m_regs[reg_hour] & 0x80) | binary_to_BCD(m_regs[reg_hour] & 0x7f);
		/*m_regs[reg_weekday] = binary_to_BCD(m_regs[reg_weekday]);*/
		m_regs[reg_monthday] = binary_to_BCD(m_regs[reg_monthday]);
		m_regs[reg_month] = binary_to_BCD(m_regs[reg_month]);
		m_regs[reg_year] = binary_to_BCD(m_regs[reg_year]);
	}
}

uint8_t rtc65271_device::read(int xramsel, offs_t offset)
{
	int reply;

	if (xramsel)
	{
		if (offset & 0x20)
			/* XRAM page register */
			reply = m_cur_xram_page;
		else
			/* XRAM data */
			reply = m_xram[(offset & 0x1f) + 0x0020*m_cur_xram_page];
	}
	else
	{
		if (offset & 0x01)
			/* data register */
			switch (m_cur_reg)
			{
			case reg_A:
				reply = m_regs[m_cur_reg] & ~reg_A_DV;
				reply |= 0x20;  // indicate normal RTC operation
				break;

			case reg_C:
				reply = m_regs[m_cur_reg];
				m_regs[m_cur_reg] = 0;
				field_interrupts();
				break;
			case reg_D:
				reply = m_regs[m_cur_reg];
				m_regs[m_cur_reg] = /*0*/reg_D_VRT; /* set VRT flag so that the computer does not complain that the battery is low */
				break;

			default:
				reply = m_regs[m_cur_reg];
				break;
			}
		else
			/* indirect address register */
			reply = m_cur_reg;
	}

	return reply;
}

void rtc65271_device::write(int xramsel, offs_t offset, uint8_t data)
{
	if (xramsel)
	{
		if (offset & 0x20)
			/* XRAM page register */
			m_cur_xram_page = data & 0x7f;
		else
			/* XRAM data */
			m_xram[(offset & 0x1f) + 0x0020*m_cur_xram_page] = data;
	}
	else
	{
		if (offset & 0x01)
			/* data register */
			switch (m_cur_reg)
			{
			case reg_second:
				/* the data sheet says bit 7 is read-only.  (I have no idea of
				the reason why it is.) */
				m_regs[reg_second] = data & 0x7f;
				break;

			case reg_A:
				if ((data & reg_A_RS) != (m_regs[m_cur_reg] & reg_A_RS))
				{
					if (data & reg_A_RS)
					{
						attotime period = attotime::from_hz(SQW_freq_table[data & reg_A_RS]);
						attotime half_period = period / 2;
						attotime elapsed = m_begin_update_timer->elapsed();

						if (half_period > elapsed)
							m_SQW_timer->adjust(half_period - elapsed);
						else
							m_SQW_timer->adjust(half_period);
					}
					else
					{
						m_SQW_internal_state = 0;   /* right??? */

						/* Stop the divider used for SQW and periodic interrupts. */
						m_SQW_timer->adjust(attotime::never);
					}
				}
				/* The UIP bit is read-only */
				m_regs[reg_A] = (data & ~reg_A_UIP) | (m_regs[reg_A] & reg_A_UIP);
				break;

			case reg_B:
				m_regs[m_cur_reg] = data;
				if (data & reg_B_SET)
				{
					/* if we are in SET mode, clear update cycle */
					m_regs[reg_A] &= ~reg_A_UIP;
					m_regs[reg_B] &= ~reg_B_UIE;    /* the data sheet tells this, but I wonder how much sense it makes */
					field_interrupts();
				}
				break;

			case reg_C:
			case reg_D:
				break;

			default:
				m_regs[m_cur_reg] = data;
				break;
			}
		else
			/* indirect address register */
			m_cur_reg = data & 0x3f;
	}
}

void rtc65271_device::field_interrupts()
{
	if (m_regs[reg_C] & m_regs[reg_B] & (reg_C_PF | reg_C_AF | reg_C_UF))
	{
		m_regs[reg_C] |= reg_C_IRQF;
		m_interrupt_cb(1);
	}
	else
	{
		m_regs[reg_C] &= ~reg_C_IRQF;
		m_interrupt_cb(0);
	}
}

int rtc65271_device::intrq_r()
{
	return (m_regs[reg_C] & reg_C_IRQF)? ASSERT_LINE : CLEAR_LINE;
}

void rtc65271_device::rtc_SQW_cb(int32_t)
{
	attotime half_period;

	m_SQW_internal_state = ! m_SQW_internal_state;
	if (! m_SQW_internal_state)
	{
		/* high-to-low??? transition -> interrupt (or should it be low-to-high?) */
		m_regs[reg_C] |= reg_C_PF;
		field_interrupts();
	}

	half_period = attotime::from_hz(SQW_freq_table[m_regs[reg_A] & reg_A_RS]) / 2;
	m_SQW_timer->adjust(half_period);
}

void rtc65271_device::rtc_begin_update_cb(int32_t)
{
	if (((m_regs[reg_A] & reg_A_DV) == 0x20) && ! (m_regs[reg_B] & reg_B_SET))
	{
		m_regs[reg_A] |= reg_A_UIP;

		/* schedule end of update cycle */
		m_end_update_timer->adjust(UPDATE_CYCLE_TIME);
	}
}

void rtc65271_device::rtc_end_update_cb(int32_t)
{
	static const int days_in_month_table[12] =
	{
		31,28,31, 30,31,30,
		31,31,30, 31,30,31
	};
	uint8_t (*increment)(uint8_t data);
	int c59, c23, c12, c11, c29;

	if (! (m_regs[reg_A] & reg_A_UIP))
		/* abort if update cycle has been canceled */
		return;

	if (m_regs[reg_B] & reg_B_DM)
	{
		/* binary mode */
		increment = increment_binary;
		c59 = 59;
		c23 = 23;
		c12 = 12;
		c11 = 11;
		c29 = 29;
	}
	else
	{
		/* BCD mode */
		increment = increment_BCD;
		c59 = 0x59;
		c23 = 0x23;
		c12 = 0x12;
		c11 = 0x11;
		c29 = 0x29;
	}

	/* increment second */
	if (m_regs[reg_second] < c59)
		m_regs[reg_second] = (*increment)(m_regs[reg_second]);
	else
	{
		m_regs[reg_second] = 0;

		/* increment minute */
		if (m_regs[reg_minute] < c59)
			m_regs[reg_minute] = (*increment)(m_regs[reg_minute]);
		else
		{
			m_regs[reg_minute] = 0;

			/* increment hour */
			if (m_regs[reg_B] & reg_B_24h)
			{
				/* 24 hour mode */
				if (m_regs[reg_hour] < c23)
					m_regs[reg_hour] = (*increment)(m_regs[reg_hour]);
				else
					m_regs[reg_hour] = 0;
			}
			else
			{
				/* 12 hour mode */
				if (m_regs[reg_hour] < c12)
				{
					if ((m_regs[reg_hour] & 0x7f) == c11)
						m_regs[reg_hour] ^= 0x80;
					m_regs[reg_hour] = ((*increment)(m_regs[reg_hour] & 0x7f) & 0x7f)
											| (m_regs[reg_hour] & 0x80);
				}
				else
					m_regs[reg_hour] = 1 | (m_regs[reg_hour] & 0x80);
			}

			/* increment day if needed */
			if (m_regs[reg_hour] == ((m_regs[reg_B] & reg_B_24h) ? 0 : c12))
			{
				/* increment day */
				int days_in_month;

				if (m_regs[reg_weekday] < 7)
					m_regs[reg_weekday]++;
				else
					m_regs[reg_weekday] = 1;

				if ((m_regs[reg_month] != 2) || (m_regs[reg_year] & 0x03))
				{
					if (m_regs[reg_B] & reg_B_DM)
					{
						/* binary mode */
						const int month_index = m_regs[reg_month] - 1;
						if (month_index < 0 || month_index >= 12) { faults |= 4; return; }
						days_in_month = days_in_month_table[month_index];
					}
					else
					{
						/* BCD mode */
						const int month_index = BCD_to_binary(m_regs[reg_month]) - 1;
						if (month_index < 0 || month_index >= 12) { faults |= 4; return; }
						days_in_month = binary_to_BCD(days_in_month_table[month_index]);
					}
				}
				else
					days_in_month = c29;

				if (m_regs[reg_monthday] < days_in_month)
					m_regs[reg_monthday] = (*increment)(m_regs[reg_monthday]);
				else
				{
					/* increment month */
					m_regs[reg_monthday] = 1;

					if (m_regs[reg_month] < c12)
						m_regs[reg_month] = (*increment)(m_regs[reg_month]);
					else
					{
						/* increment year */
						m_regs[reg_month] = 1;

						if (m_regs[reg_B] & reg_B_DM)
						{
							/* binary mode */
							if (m_regs[reg_year] < 99)
								m_regs[reg_year]++;
							else
								m_regs[reg_year] = 0;
						}
						else
						{
							/* BCD mode */
							m_regs[reg_year] = increment_BCD(m_regs[reg_year]);
						}
					}
				}
			}
		}
	}

	m_regs[reg_A] &= ~reg_A_UIP;
	m_regs[reg_C] |= reg_C_UF;

	/* test for alarm (values in range 0xc0-0xff mean "don't care") */
	if ((((m_regs[reg_alarm_second] & 0xc0) == 0xc0) || (m_regs[reg_alarm_second] == m_regs[reg_second]))
			&& (((m_regs[reg_alarm_minute] & 0xc0) == 0xc0) || (m_regs[reg_alarm_minute] == m_regs[reg_minute]))
			&& (((m_regs[reg_alarm_hour] & 0xc0) == 0xc0) || (m_regs[reg_alarm_hour] == m_regs[reg_hour])))
		m_regs[reg_C] |= reg_C_AF;

	field_interrupts();
}
