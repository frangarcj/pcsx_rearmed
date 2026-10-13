/* Generated from MAME sh.cpp/sh2.cpp, deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright David Haywood and the SH-2 contributors. BSD-3-Clause; sh2_LICENSE.txt. */
void StandaloneSh2::ADD(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] += m_sh2_state->r[m];
}

/*  code                 cycles  t-bit
 *  0111 nnnn iiii iiii  1       -
 *  ADD     #imm,Rn
 */
void StandaloneSh2::ADDI(uint32_t i, uint32_t n)
{
	m_sh2_state->r[n] += util::sext(i, 8);
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 1110  1       carry
 *  ADDC    Rm,Rn
 */
void StandaloneSh2::ADDC(uint32_t m, uint32_t n)
{
	uint32_t tmp0, tmp1;

	tmp1 = m_sh2_state->r[n] + m_sh2_state->r[m];
	tmp0 = m_sh2_state->r[n];
	m_sh2_state->r[n] = tmp1 + (m_sh2_state->sr & SH_T);
	if (tmp0 > tmp1)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
	if (tmp1 > m_sh2_state->r[n])
		m_sh2_state->sr |= SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 1111  1       overflow
 *  ADDV    Rm,Rn
 */
void StandaloneSh2::ADDV(uint32_t m, uint32_t n)
{
	int32_t dest = BIT(m_sh2_state->r[n], 31);
	int32_t src = BIT(m_sh2_state->r[m], 31);
	src += dest;

	m_sh2_state->r[n] += m_sh2_state->r[m];

	int32_t ans = BIT(m_sh2_state->r[n], 31);
	ans += dest;

	if (src != 1)
	{
		if (ans == 1)
			m_sh2_state->sr |= SH_T;
		else
			m_sh2_state->sr &= ~SH_T;
	}
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0010 nnnn mmmm 1001  1       -
 *  AND     Rm,Rn
 */
void StandaloneSh2::AND(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] &= m_sh2_state->r[m];
}

/*  code                 cycles  t-bit
 *  1100 1001 iiii iiii  1       -
 *  AND     #imm,R0
 */
void StandaloneSh2::ANDI(uint32_t i)
{
	m_sh2_state->r[0] &= i;
}

/*  code                 cycles  t-bit
 *  1100 1101 iiii iiii  1       -
 *  AND.B   #imm,@(R0,GBR)
 */
void StandaloneSh2::ANDM(uint32_t i)
{
	m_sh2_state->ea = m_sh2_state->gbr + m_sh2_state->r[0];
	uint32_t temp = i & read_byte(m_sh2_state->ea);
	write_byte(m_sh2_state->ea, temp);
	m_sh2_state->icount -= 2;
}

/*  code                 cycles  t-bit
 *  1000 1011 dddd dddd  3/1     -
 *  BF      disp8
 */
void StandaloneSh2::BF(uint32_t d)
{
	if ((m_sh2_state->sr & SH_T) == 0)
	{
		int32_t disp = util::sext(d, 8);
		m_sh2_state->pc = m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
		m_sh2_state->icount -= 2;
	}
}

/*  code                 cycles  t-bit
 *  1000 1111 dddd dddd  3/1     -
 *  BFS     disp8
 */
void StandaloneSh2::BFS(uint32_t d)
{
	if ((m_sh2_state->sr & SH_T) == 0)
	{
		int32_t disp = util::sext(d, 8);
		m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
		m_sh2_state->icount--;
	}
}

/*  code                 cycles  t-bit
 *  1010 dddd dddd dddd  2       -
 *  BRA     disp12
 */
void StandaloneSh2::BRA(uint32_t d)
{
	int32_t disp = util::sext(d, 12);

#if BUSY_LOOP_HACKS
	if (disp == -2)
	{
		uint32_t next_opcode = read_word(m_sh2_state->pc & m_am);
		/* BRA  $
		 * NOP
		 */
		if (next_opcode == 0x0009)
			m_sh2_state->icount %= 3;   /* cycles for BRA $ and NOP taken (3) */
	}
#endif
	m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
	m_sh2_state->icount--;
}

/*  code                 cycles  t-bit
 *  0000 mmmm 0010 0011  2       -
 *  BRAF    Rm
 */
void StandaloneSh2::BRAF(uint32_t m)
{
	m_sh2_state->m_delay = m_sh2_state->pc + m_sh2_state->r[m] + 2;
	m_sh2_state->icount--;
}

/*  code                 cycles  t-bit
 *  1011 dddd dddd dddd  2       -
 *  BSR     disp12
 */
void StandaloneSh2::BSR(uint32_t d)
{
	int32_t disp = util::sext(d, 12);

	m_sh2_state->pr = m_sh2_state->pc + 2;
	m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
	m_sh2_state->icount--;
}

/*  code                 cycles  t-bit
 *  0000 mmmm 0000 0011  2       -
 *  BSRF    Rm
 */
void StandaloneSh2::BSRF(uint32_t m)
{
	m_sh2_state->pr = m_sh2_state->pc + 2;
	m_sh2_state->m_delay = m_sh2_state->pc + m_sh2_state->r[m] + 2;
	m_sh2_state->icount--;
}

/*  code                 cycles  t-bit
 *  1000 1001 dddd dddd  3/1     -
 *  BT      disp8
 */
void StandaloneSh2::BT(uint32_t d)
{
	if ((m_sh2_state->sr & SH_T) != 0)
	{
		int32_t disp = util::sext(d, 8);
		m_sh2_state->pc = m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
		m_sh2_state->icount -= 2;
	}
}

/*  code                 cycles  t-bit
 *  1000 1101 dddd dddd  2/1     -
 *  BTS     disp8
 */
void StandaloneSh2::BTS(uint32_t d)
{
	if ((m_sh2_state->sr & SH_T) != 0)
	{
		int32_t disp = util::sext(d, 8);
		m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
		m_sh2_state->icount--;
	}
}

/*  code                 cycles  t-bit
 *  0000 0000 0010 1000  1       -
 *  CLRMAC
 */
void StandaloneSh2::CLRMAC()
{
	m_sh2_state->mach = 0;
	m_sh2_state->macl = 0;
}

/*  code                 cycles  t-bit
 *  0000 0000 0000 1000  1       -
 *  CLRT
 */
void StandaloneSh2::CLRT()
{
	m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0000  1       comparison result
 *  CMP_EQ  Rm,Rn
 */
void StandaloneSh2::CMPEQ(uint32_t m, uint32_t n)
{
	if (m_sh2_state->r[n] == m_sh2_state->r[m])
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0011  1       comparison result
 *  CMP_GE  Rm,Rn
 */
void StandaloneSh2::CMPGE(uint32_t m, uint32_t n)
{
	if ((int32_t)m_sh2_state->r[n] >= (int32_t)m_sh2_state->r[m])
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0111  1       comparison result
 *  CMP_GT  Rm,Rn
 */
void StandaloneSh2::CMPGT(uint32_t m, uint32_t n)
{
	if ((int32_t)m_sh2_state->r[n] > (int32_t)m_sh2_state->r[m])
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0110  1       comparison result
 *  CMP_HI  Rm,Rn
 */
void StandaloneSh2::CMPHI(uint32_t m, uint32_t n)
{
	if (m_sh2_state->r[n] > m_sh2_state->r[m])
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0010  1       comparison result
 *  CMP_HS  Rm,Rn
 */
void StandaloneSh2::CMPHS(uint32_t m, uint32_t n)
{
	if (m_sh2_state->r[n] >= m_sh2_state->r[m])
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0100 nnnn 0001 0101  1       comparison result
 *  CMP_PL  Rn
 */
void StandaloneSh2::CMPPL(uint32_t n)
{
	if ((int32_t)m_sh2_state->r[n] > 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0100 nnnn 0001 0001  1       comparison result
 *  CMP_PZ  Rn
 */
void StandaloneSh2::CMPPZ(uint32_t n)
{
	if ((int32_t)m_sh2_state->r[n] >= 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0010 nnnn mmmm 1100  1       comparison result
 * CMP_STR  Rm,Rn
 */
void StandaloneSh2::CMPSTR(uint32_t m, uint32_t n)
{
	uint32_t temp = m_sh2_state->r[n] ^ m_sh2_state->r[m];
	uint8_t upper_byte = (temp >> 24) & 0xff;
	uint8_t mid_upper_byte = (temp >> 16) & 0xff;
	uint8_t mid_lower_byte = (temp >> 8) & 0xff;
	uint8_t lower_byte = temp & 0xff;
	if (upper_byte && mid_upper_byte && mid_lower_byte && lower_byte)
		m_sh2_state->sr &= ~SH_T;
	else
		m_sh2_state->sr |= SH_T;
}

/*  code                 cycles  t-bit
 *  1000 1000 iiii iiii  1       comparison result
 *  CMP/EQ #imm,R0
 */
void StandaloneSh2::CMPIM(uint32_t i)
{
	uint32_t imm = (uint32_t)util::sext(i, 8);

	if (m_sh2_state->r[0] == imm)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0010 nnnn mmmm 0111  1       calculation result
 *  DIV0S   Rm,Rn
 */
void StandaloneSh2::DIV0S(uint32_t m, uint32_t n)
{
	if (!BIT(m_sh2_state->r[n], 31))
		m_sh2_state->sr &= ~SH_Q;
	else
		m_sh2_state->sr |= SH_Q;

	if (!BIT(m_sh2_state->r[m], 31))
		m_sh2_state->sr &= ~SH_M;
	else
		m_sh2_state->sr |= SH_M;

	if (BIT(m_sh2_state->r[m] ^ m_sh2_state->r[n], 31))
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0000 0000 0001 1001  1       0
 *  DIV0U
 */
void StandaloneSh2::DIV0U()
{
	m_sh2_state->sr &= ~(SH_M | SH_Q | SH_T);
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0100  1       calculation result
 *  DIV1 Rm,Rn
 */
void StandaloneSh2::DIV1(uint32_t m, uint32_t n)
{
	uint32_t old_q = m_sh2_state->sr & SH_Q;
	if (0x80000000 & m_sh2_state->r[n])
		m_sh2_state->sr |= SH_Q;
	else
		m_sh2_state->sr &= ~SH_Q;

	m_sh2_state->r[n] = (m_sh2_state->r[n] << 1) | (m_sh2_state->sr & SH_T);

	if (!old_q)
	{
		if (!(m_sh2_state->sr & SH_M))
		{
			uint32_t tmp = m_sh2_state->r[n];
			m_sh2_state->r[n] -= m_sh2_state->r[m];
			if (!(m_sh2_state->sr & SH_Q))
				if (m_sh2_state->r[n] > tmp)
					m_sh2_state->sr |= SH_Q;
				else
					m_sh2_state->sr &= ~SH_Q;
			else
				if (m_sh2_state->r[n] > tmp)
					m_sh2_state->sr &= ~SH_Q;
				else
					m_sh2_state->sr |= SH_Q;
		}
		else
		{
			uint32_t tmp = m_sh2_state->r[n];
			m_sh2_state->r[n] += m_sh2_state->r[m];
			if (!(m_sh2_state->sr & SH_Q))
			{
				if (m_sh2_state->r[n] < tmp)
					m_sh2_state->sr &= ~SH_Q;
				else
					m_sh2_state->sr |= SH_Q;
			}
			else
			{
				if (m_sh2_state->r[n] < tmp)
					m_sh2_state->sr |= SH_Q;
				else
					m_sh2_state->sr &= ~SH_Q;
			}
		}
	}
	else
	{
		if (!(m_sh2_state->sr & SH_M))
		{
			uint32_t tmp = m_sh2_state->r[n];
			m_sh2_state->r[n] += m_sh2_state->r[m];
			if (!(m_sh2_state->sr & SH_Q))
				if (m_sh2_state->r[n] < tmp)
					m_sh2_state->sr |= SH_Q;
				else
					m_sh2_state->sr &= ~SH_Q;
			else
				if (m_sh2_state->r[n] < tmp)
					m_sh2_state->sr &= ~SH_Q;
				else
					m_sh2_state->sr |= SH_Q;
		}
		else
		{
			uint32_t tmp = m_sh2_state->r[n];
			m_sh2_state->r[n] -= m_sh2_state->r[m];
			if (!(m_sh2_state->sr & SH_Q))
				if (m_sh2_state->r[n] > tmp)
					m_sh2_state->sr &= ~SH_Q;
				else
					m_sh2_state->sr |= SH_Q;
			else
				if (m_sh2_state->r[n] > tmp)
					m_sh2_state->sr |= SH_Q;
				else
					m_sh2_state->sr &= ~SH_Q;
		}
	}

	uint32_t tmp = (m_sh2_state->sr & (SH_Q | SH_M));
	if (tmp == 0 || tmp == 0x300) /* if Q == M set T else clear T */
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  DMULS.L Rm,Rn */
void StandaloneSh2::DMULS(uint32_t m, uint32_t n)
{
	int32_t tempn = (int32_t)m_sh2_state->r[n];
	int32_t tempm = (int32_t)m_sh2_state->r[m];
	bool fnlml = (bool)BIT(tempn ^ tempm, 31);

	if (tempn < 0)
		tempn = 0 - tempn;
	if (tempm < 0)
		tempm = 0 - tempm;

	uint32_t rn_l = (uint32_t)tempn & 0x0000ffff;
	uint32_t rn_h = (uint32_t)tempn >> 16;
	uint32_t rm_l = (uint32_t)tempm & 0x0000ffff;
	uint32_t rm_h = (uint32_t)tempm >> 16;

	uint32_t temp0 = rm_l * rn_l;
	uint32_t temp1 = rm_h * rn_l;
	uint32_t temp2 = rm_l * rn_h;
	uint32_t temp3 = rm_h * rn_h;

	uint32_t res2 = 0;
	uint32_t res1 = temp1 + temp2;
	if (res1 < temp1)
		res2 += 0x00010000;
	temp1 = res1 << 16;

	uint32_t res0 = temp0 + temp1;
	if (res0 < temp0)
		res2++;
	res2 = res2 + (res1 >> 16) + temp3;

	if (fnlml)
	{
		res2 = ~res2;
		if (res0 == 0)
			res2++;
		else
			res0 = (~res0) + 1;
	}

	m_sh2_state->mach = res2;
	m_sh2_state->macl = res0;
	m_sh2_state->icount--;
}

/*  DMULU.L Rm,Rn */
void StandaloneSh2::DMULU(uint32_t m, uint32_t n)
{
	uint32_t rn_l = m_sh2_state->r[n] & 0x0000ffff;
	uint32_t rn_h = m_sh2_state->r[n] >> 16;
	uint32_t rm_l = m_sh2_state->r[m] & 0x0000ffff;
	uint32_t rm_h = m_sh2_state->r[m] >> 16;

	uint32_t temp0 = rm_l * rn_l;
	uint32_t temp1 = rm_h * rn_l;
	uint32_t temp2 = rm_l * rn_h;
	uint32_t temp3 = rm_h * rn_h;

	uint32_t res2 = 0;
	uint32_t res1 = temp1 + temp2;
	if (res1 < temp1)
		res2 += 0x00010000;

	temp1 = res1 << 16;
	uint32_t res0 = temp0 + temp1;
	if (res0 < temp0)
		res2++;

	res2 = res2 + (res1 >> 16) + temp3;

	m_sh2_state->mach = res2;
	m_sh2_state->macl = res0;
	m_sh2_state->icount--;
}

/*  DT      Rn */
void StandaloneSh2::DT(uint32_t n)
{
	m_sh2_state->r[n]--;
	if (m_sh2_state->r[n] == 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
#if BUSY_LOOP_HACKS
	{
		uint32_t next_opcode = read_word(m_sh2_state->pc & AM);
		/* DT   Rn
		 * BF   $-2
		 */
		if (next_opcode == 0x8bfd)
		{
			while (m_sh2_state->r[n] > 1 && m_sh2_state->icount > 4)
			{
				m_sh2_state->r[n]--;
				m_sh2_state->icount -= 4;   /* cycles for DT (1) and BF taken (3) */
			}
		}
	}
#endif
}

/*  EXTS.B  Rm,Rn */
void StandaloneSh2::EXTSB(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = util::sext(m_sh2_state->r[m], 8);
}

/*  EXTS.W  Rm,Rn */
void StandaloneSh2::EXTSW(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = util::sext(m_sh2_state->r[m], 16);
}

/*  EXTU.B  Rm,Rn */
void StandaloneSh2::EXTUB(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->r[m] & 0x000000ff;
}

/*  EXTU.W  Rm,Rn */
void StandaloneSh2::EXTUW(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->r[m] & 0x0000ffff;
}

/*  JMP     @Rm */
void StandaloneSh2::JMP(uint32_t m)
{
	m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->r[m];
	//m_sh2_state->icount--; // not in SH4 implementation?
}

/*  JSR     @Rm */
void StandaloneSh2::JSR(uint32_t m)
{
	m_sh2_state->pr = m_sh2_state->pc + 2;
	m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->icount--;
}

/*  LDC     Rm,GBR */
void StandaloneSh2::LDCGBR(uint32_t m)
{
	m_sh2_state->gbr = m_sh2_state->r[m];
}

/*  LDC     Rm,VBR */
void StandaloneSh2::LDCVBR(uint32_t m)
{
	m_sh2_state->vbr = m_sh2_state->r[m];
}

/*  LDC.L   @Rm+,GBR */
void StandaloneSh2::LDCMGBR(uint32_t m)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->gbr = read_long(m_sh2_state->ea);
	m_sh2_state->r[m] += 4;
	m_sh2_state->icount -= 2;
}

/*  LDC.L   @Rm+,VBR */
void StandaloneSh2::LDCMVBR(uint32_t m)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->vbr = read_long(m_sh2_state->ea);
	m_sh2_state->r[m] += 4;
	m_sh2_state->icount -= 2;
}

/*  LDS     Rm,MACH */
void StandaloneSh2::LDSMACH(uint32_t m)
{
	m_sh2_state->mach = m_sh2_state->r[m];
}

/*  LDS     Rm,MACL */
void StandaloneSh2::LDSMACL(uint32_t m)
{
	m_sh2_state->macl = m_sh2_state->r[m];
}

/*  LDS     Rm,PR */
void StandaloneSh2::LDSPR(uint32_t m)
{
	m_sh2_state->pr = m_sh2_state->r[m];
}

/*  LDS.L   @Rm+,MACH */
void StandaloneSh2::LDSMMACH(uint32_t m)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->mach = read_long(m_sh2_state->ea);
	m_sh2_state->r[m] += 4;
}

/*  LDS.L   @Rm+,MACL */
void StandaloneSh2::LDSMMACL(uint32_t m)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->macl = read_long(m_sh2_state->ea);
	m_sh2_state->r[m] += 4;
}

/*  LDS.L   @Rm+,PR */
void StandaloneSh2::LDSMPR(uint32_t m)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->pr = read_long(m_sh2_state->ea);
	m_sh2_state->r[m] += 4;
}

/*  MAC.L   @Rm+,@Rn+ */
void StandaloneSh2::MAC_L(uint32_t m, uint32_t n)
{
	int32_t tempn = (int32_t)read_long(m_sh2_state->r[n]);
	m_sh2_state->r[n] += 4;

	int32_t tempm = (int32_t)read_long(m_sh2_state->r[m]);
	m_sh2_state->r[m] += 4;

	bool fnlml = BIT(tempn ^ tempm, 31);

	if (tempn < 0)
		tempn = 0 - tempn;
	if (tempm < 0)
		tempm = 0 - tempm;

	uint32_t rn_l = (uint32_t)tempn & 0x0000ffff;
	uint32_t rn_h = (uint32_t)tempn >> 16;
	uint32_t rm_l = (uint32_t)tempm & 0x0000ffff;
	uint32_t rm_h = (uint32_t)tempm >> 16;

	uint32_t temp0 = rm_l * rn_l;
	uint32_t temp1 = rm_h * rn_l;
	uint32_t temp2 = rm_l * rn_h;
	uint32_t temp3 = rm_h * rn_h;

	uint32_t res2 = 0;
	uint32_t res1 = temp1 + temp2;
	if (res1 < temp1)
		res2 += 0x00010000;
	temp1 = res1 << 16;

	uint32_t res0 = temp0 + temp1;
	if (res0 < temp0)
		res2++;
	res2 = res2 + (res1 >> 16) + temp3;

	if (fnlml)
	{
		res2 = ~res2;
		if (res0 == 0)
			res2++;
		else
			res0 = (~res0) + 1;
	}

	if (m_sh2_state->sr & SH_S)
	{
		res0 = m_sh2_state->macl + res0;
		if (m_sh2_state->macl > res0)
			res2++;
		res2 += m_sh2_state->mach & 0x0000ffff;
		if ((int32_t)res2 < 0 && res2 < 0xffff8000)
		{
			res2 = 0x00008000;
			res0 = 0x00000000;
		}
		else if ((int32_t)res2 > 0 && res2 > 0x00007fff)
		{
			res2 = 0x00007fff;
			res0 = 0xffffffff;
		}
		m_sh2_state->mach = res2;
		m_sh2_state->macl = res0;
	}
	else
	{
		res0 = m_sh2_state->macl + res0;
		if (m_sh2_state->macl > res0)
			res2++;
		res2 += m_sh2_state->mach;
		m_sh2_state->mach = res2;
		m_sh2_state->macl = res0;
	}
	m_sh2_state->icount -= 2;
}

/*  MAC.W   @Rm+,@Rn+ */
void StandaloneSh2::MAC_W(uint32_t m, uint32_t n)
{
	int32_t tempn = (int32_t)(int16_t)read_word(m_sh2_state->r[n]);
	m_sh2_state->r[n] += 2;

	int32_t tempm = (int32_t)(int16_t)read_word(m_sh2_state->r[m]);
	m_sh2_state->r[m] += 2;

	uint32_t templ = m_sh2_state->macl;
	tempm *= tempn;

	int32_t dest = BIT(m_sh2_state->macl, 31);
	int32_t src = BIT(tempm, 31) + dest;
	tempn = BIT(tempm, 31) ? -1 : 0;

	m_sh2_state->macl += tempm;

	int32_t ans = BIT(m_sh2_state->macl, 31) + dest;

	if (m_sh2_state->sr & SH_S)
	{
		if (ans == 1)
		{
			if (src == 0)
				m_sh2_state->macl = 0x7fffffff;
			else if (src == 2)
				m_sh2_state->macl = 0x80000000;
		}
	}
	else
	{
		m_sh2_state->mach += tempn;
		if (templ > m_sh2_state->macl)
			m_sh2_state->mach += 1;
	}
	m_sh2_state->icount -= 2;
}

/*  MOV     Rm,Rn */
void StandaloneSh2::MOV(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->r[m];
}

/*  MOV.B   Rm,@Rn */
void StandaloneSh2::MOVBS(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n];
	write_byte(m_sh2_state->ea, m_sh2_state->r[m] & 0x000000ff);
}

/*  MOV.W   Rm,@Rn */
void StandaloneSh2::MOVWS(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n];
	write_word(m_sh2_state->ea, m_sh2_state->r[m] & 0x0000ffff);
}

/*  MOV.L   Rm,@Rn */
void StandaloneSh2::MOVLS(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->r[m]);
}

/*  MOV.B   @Rm,Rn */
void StandaloneSh2::MOVBL(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->r[n] = (uint32_t)util::sext(read_byte(m_sh2_state->ea), 8);
}

/*  MOV.W   @Rm,Rn */
void StandaloneSh2::MOVWL(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->r[n] = (uint32_t)util::sext(read_word(m_sh2_state->ea), 16);
}

/*  MOV.L   @Rm,Rn */
void StandaloneSh2::MOVLL(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[m];
	m_sh2_state->r[n] = read_long(m_sh2_state->ea);
}

/*  MOV.B   Rm,@-Rn */
void StandaloneSh2::MOVBM(uint32_t m, uint32_t n)
{
	uint8_t data(m_sh2_state->r[m]);

	m_sh2_state->r[n] -= 1;
	write_byte(m_sh2_state->r[n], data);
}

/*  MOV.W   Rm,@-Rn */
void StandaloneSh2::MOVWM(uint32_t m, uint32_t n)
{
	uint16_t data(m_sh2_state->r[m]);

	m_sh2_state->r[n] -= 2;
	write_word(m_sh2_state->r[n], data);
}

/*  MOV.L   Rm,@-Rn */
void StandaloneSh2::MOVLM(uint32_t m, uint32_t n)
{
	uint32_t data = m_sh2_state->r[m];

	m_sh2_state->r[n] -= 4;
	write_long(m_sh2_state->r[n], data);
}

/*  MOV.B   @Rm+,Rn */
void StandaloneSh2::MOVBP(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = (uint32_t)util::sext(read_byte(m_sh2_state->r[m]), 8);
	if (n != m)
		m_sh2_state->r[m] += 1;
}

/*  MOV.W   @Rm+,Rn */
void StandaloneSh2::MOVWP(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = (uint32_t)util::sext(read_word(m_sh2_state->r[m]), 16);
	if (n != m)
		m_sh2_state->r[m] += 2;
}

/*  MOV.L   @Rm+,Rn */
void StandaloneSh2::MOVLP(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = read_long(m_sh2_state->r[m]);
	if (n != m)
		m_sh2_state->r[m] += 4;
}

/*  MOV.B   Rm,@(R0,Rn) */
void StandaloneSh2::MOVBS0(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n] + m_sh2_state->r[0];
	write_byte(m_sh2_state->ea, (uint8_t)m_sh2_state->r[m]);
}

/*  MOV.W   Rm,@(R0,Rn) */
void StandaloneSh2::MOVWS0(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n] + m_sh2_state->r[0];
	write_word(m_sh2_state->ea, (uint16_t)m_sh2_state->r[m]);
}

/*  MOV.L   Rm,@(R0,Rn) */
void StandaloneSh2::MOVLS0(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n] + m_sh2_state->r[0];
	write_long(m_sh2_state->ea, m_sh2_state->r[m]);
}

/*  MOV.B   @(R0,Rm),Rn */
void StandaloneSh2::MOVBL0(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[m] + m_sh2_state->r[0];
	m_sh2_state->r[n] = (uint32_t)util::sext(read_byte(m_sh2_state->ea), 8);
}

/*  MOV.W   @(R0,Rm),Rn */
void StandaloneSh2::MOVWL0(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[m] + m_sh2_state->r[0];
	m_sh2_state->r[n] = (uint32_t)util::sext(read_word(m_sh2_state->ea), 16);
}

/*  MOV.L   @(R0,Rm),Rn */
void StandaloneSh2::MOVLL0(uint32_t m, uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[m] + m_sh2_state->r[0];
	m_sh2_state->r[n] = read_long(m_sh2_state->ea);
}

/*  MOV     #imm,Rn */
void StandaloneSh2::MOVI(uint32_t i, uint32_t n)
{
	m_sh2_state->r[n] = (uint32_t)util::sext(i, 8);
}

/*  MOV.W   @(disp8,PC),Rn */
void StandaloneSh2::MOVWI(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->pc + disp * 2 + 2;
	m_sh2_state->r[n] = (uint32_t)util::sext(read_word(m_sh2_state->ea), 16);
}

/*  MOV.L   @(disp8,PC),Rn */
void StandaloneSh2::MOVLI(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = ((m_sh2_state->pc + 2) & ~3) + disp * 4;
	m_sh2_state->r[n] = read_long(m_sh2_state->ea);
}

/*  MOV.B   @(disp8,GBR),R0 */
void StandaloneSh2::MOVBLG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->gbr + disp;
	m_sh2_state->r[0] = (uint32_t)util::sext(read_byte(m_sh2_state->ea), 8);
}

/*  MOV.W   @(disp8,GBR),R0 */
void StandaloneSh2::MOVWLG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->gbr + disp * 2;
	m_sh2_state->r[0] = (int32_t)util::sext(read_word(m_sh2_state->ea), 16);
}

/*  MOV.L   @(disp8,GBR),R0 */
void StandaloneSh2::MOVLLG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->gbr + disp * 4;
	m_sh2_state->r[0] = read_long(m_sh2_state->ea);
}

/*  MOV.B   R0,@(disp8,GBR) */
void StandaloneSh2::MOVBSG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->gbr + disp;
	write_byte(m_sh2_state->ea, (uint8_t)m_sh2_state->r[0]);
}

/*  MOV.W   R0,@(disp8,GBR) */
void StandaloneSh2::MOVWSG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->gbr + disp * 2;
	write_word(m_sh2_state->ea, (uint16_t)m_sh2_state->r[0]);
}

/*  MOV.L   R0,@(disp8,GBR) */
void StandaloneSh2::MOVLSG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = m_sh2_state->gbr + disp * 4;
	write_long(m_sh2_state->ea, m_sh2_state->r[0]);
}

/*  MOV.B   R0,@(disp4,Rn) */
void StandaloneSh2::MOVBS4(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	m_sh2_state->ea = m_sh2_state->r[n] + disp;
	write_byte(m_sh2_state->ea, (uint8_t)m_sh2_state->r[0]);
}

/*  MOV.W   R0,@(disp4,Rn) */
void StandaloneSh2::MOVWS4(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	m_sh2_state->ea = m_sh2_state->r[n] + disp * 2;
	write_word(m_sh2_state->ea, (uint16_t)m_sh2_state->r[0]);
}

/* MOV.L Rm,@(disp4,Rn) */
void StandaloneSh2::MOVLS4(uint32_t m, uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	m_sh2_state->ea = m_sh2_state->r[n] + disp * 4;
	write_long(m_sh2_state->ea, m_sh2_state->r[m]);
}

/*  MOV.B   @(disp4,Rm),R0 */
void StandaloneSh2::MOVBL4(uint32_t m, uint32_t d)
{
	uint32_t disp = d & 0x0f;
	m_sh2_state->ea = m_sh2_state->r[m] + disp;
	m_sh2_state->r[0] = (uint32_t)util::sext(read_byte(m_sh2_state->ea), 8);
}

/*  MOV.W   @(disp4,Rm),R0 */
void StandaloneSh2::MOVWL4(uint32_t m, uint32_t d)
{
	uint32_t disp = d & 0x0f;
	m_sh2_state->ea = m_sh2_state->r[m] + disp * 2;
	m_sh2_state->r[0] = (uint32_t)util::sext(read_word(m_sh2_state->ea), 16);
}

/*  MOV.L   @(disp4,Rm),Rn */
void StandaloneSh2::MOVLL4(uint32_t m, uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	m_sh2_state->ea = m_sh2_state->r[m] + disp * 4;
	m_sh2_state->r[n] = read_long(m_sh2_state->ea);
}

/*  MOVA    @(disp8,PC),R0 */
void StandaloneSh2::MOVA(uint32_t d)
{
	uint32_t disp = d & 0xff;
	m_sh2_state->ea = ((m_sh2_state->pc + 2) & ~3) + disp * 4;
	m_sh2_state->r[0] = m_sh2_state->ea;
}

/*  MOVT    Rn */
void StandaloneSh2::MOVT(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->sr & SH_T;
}

/*  MUL.L   Rm,Rn */
void StandaloneSh2::MULL(uint32_t m, uint32_t n)
{
	m_sh2_state->macl = m_sh2_state->r[n] * m_sh2_state->r[m];
	m_sh2_state->icount--;
}

/*  MULS    Rm,Rn */
void StandaloneSh2::MULS(uint32_t m, uint32_t n)
{
	m_sh2_state->macl = (int16_t)m_sh2_state->r[n] * (int16_t)m_sh2_state->r[m];
}

/*  MULU    Rm,Rn */
void StandaloneSh2::MULU(uint32_t m, uint32_t n)
{
	m_sh2_state->macl = (uint16_t)m_sh2_state->r[n] * (uint16_t)m_sh2_state->r[m];
}

/*  NEG     Rm,Rn */
void StandaloneSh2::NEG(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = 0 - m_sh2_state->r[m];
}

/*  NEGC    Rm,Rn */
void StandaloneSh2::NEGC(uint32_t m, uint32_t n)
{
	uint32_t temp = m_sh2_state->r[m];
	m_sh2_state->r[n] = -temp - (m_sh2_state->sr & SH_T);
	if (temp || (m_sh2_state->sr & SH_T))
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  NOP */
void StandaloneSh2::NOP(void)
{
}

/*  NOT     Rm,Rn */
void StandaloneSh2::NOT(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = ~m_sh2_state->r[m];
}

/*  OR      Rm,Rn */
void StandaloneSh2::OR(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] |= m_sh2_state->r[m];
}

/*  OR      #imm,R0 */
void StandaloneSh2::ORI(uint32_t i)
{
	m_sh2_state->r[0] |= i;
	m_sh2_state->icount -= 2;
}

/*  OR.B    #imm,@(R0,GBR) */
void StandaloneSh2::ORM(uint32_t i)
{
	m_sh2_state->ea = m_sh2_state->gbr + m_sh2_state->r[0];
	write_byte(m_sh2_state->ea, read_byte(m_sh2_state->ea) | (uint8_t)i);
}

/*  ROTCL   Rn */
void StandaloneSh2::ROTCL(uint32_t n)
{
	uint32_t temp = (m_sh2_state->r[n] >> 31) & SH_T;
	m_sh2_state->r[n] = (m_sh2_state->r[n] << 1) | (m_sh2_state->sr & SH_T);
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | temp;
}

/*  ROTCR   Rn */
void StandaloneSh2::ROTCR(uint32_t n)
{
	uint32_t temp = (m_sh2_state->sr & SH_T) << 31;
	if (m_sh2_state->r[n] & SH_T)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
	m_sh2_state->r[n] = (m_sh2_state->r[n] >> 1) | temp;
}

/*  ROTL    Rn */
void StandaloneSh2::ROTL(uint32_t n)
{
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | ((m_sh2_state->r[n] >> 31) & SH_T);
	m_sh2_state->r[n] = rotl_32(m_sh2_state->r[n], 1);
}

/*  ROTR    Rn */
void StandaloneSh2::ROTR(uint32_t n)
{
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | (m_sh2_state->r[n] & SH_T);
	m_sh2_state->r[n] = rotr_32(m_sh2_state->r[n], 1);
}

/*  RTS */
void StandaloneSh2::RTS()
{
	m_sh2_state->m_delay = m_sh2_state->ea = m_sh2_state->pr;
	m_sh2_state->icount--;
}

/*  SETT */
void StandaloneSh2::SETT()
{
	m_sh2_state->sr |= SH_T;
}

/*  SHAL    Rn      (same as SHLL) */
void StandaloneSh2::SHAL(uint32_t n)
{
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | ((m_sh2_state->r[n] >> 31) & SH_T);
	m_sh2_state->r[n] <<= 1;
}

/*  SHAR    Rn */
void StandaloneSh2::SHAR(uint32_t n)
{
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | (m_sh2_state->r[n] & SH_T);
	m_sh2_state->r[n] = (uint32_t)((int32_t)m_sh2_state->r[n] >> 1);
}

/*  SHLL    Rn      (same as SHAL) */
void StandaloneSh2::SHLL(uint32_t n)
{
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | ((m_sh2_state->r[n] >> 31) & SH_T);
	m_sh2_state->r[n] <<= 1;
}

/*  SHLL2   Rn */
void StandaloneSh2::SHLL2(uint32_t n)
{
	m_sh2_state->r[n] <<= 2;
}

/*  SHLL8   Rn */
void StandaloneSh2::SHLL8(uint32_t n)
{
	m_sh2_state->r[n] <<= 8;
}

/*  SHLL16  Rn */
void StandaloneSh2::SHLL16(uint32_t n)
{
	m_sh2_state->r[n] <<= 16;
}

/*  SHLR    Rn */
void StandaloneSh2::SHLR(uint32_t n)
{
	m_sh2_state->sr = (m_sh2_state->sr & ~SH_T) | (m_sh2_state->r[n] & SH_T);
	m_sh2_state->r[n] >>= 1;
}

/*  SHLR2   Rn */
void StandaloneSh2::SHLR2(uint32_t n)
{
	m_sh2_state->r[n] >>= 2;
}

/*  SHLR8   Rn */
void StandaloneSh2::SHLR8(uint32_t n)
{
	m_sh2_state->r[n] >>= 8;
}

/*  SHLR16  Rn */
void StandaloneSh2::SHLR16(uint32_t n)
{
	m_sh2_state->r[n] >>= 16;
}


/*  STC     SR,Rn */
void StandaloneSh2::STCSR(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->sr;
}

/*  STC     GBR,Rn */
void StandaloneSh2::STCGBR(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->gbr;
}

/*  STC     VBR,Rn */
void StandaloneSh2::STCVBR(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->vbr;
}

/*  STC.L   SR,@-Rn */
void StandaloneSh2::STCMSR(uint32_t n)
{
	m_sh2_state->r[n] -= 4;
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->sr);
	m_sh2_state->icount--;
}

/*  STC.L   GBR,@-Rn */
void StandaloneSh2::STCMGBR(uint32_t n)
{
	m_sh2_state->r[n] -= 4;
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->gbr);
	m_sh2_state->icount--;
}

/*  STC.L   VBR,@-Rn */
void StandaloneSh2::STCMVBR(uint32_t n)
{
	m_sh2_state->r[n] -= 4;
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->vbr);
	m_sh2_state->icount--;
}

/*  STS     MACH,Rn */
void StandaloneSh2::STSMACH(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->mach;
}

/*  STS     MACL,Rn */
void StandaloneSh2::STSMACL(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->macl;
}

/*  STS     PR,Rn */
void StandaloneSh2::STSPR(uint32_t n)
{
	m_sh2_state->r[n] = m_sh2_state->pr;
}

/*  STS.L   MACH,@-Rn */
void StandaloneSh2::STSMMACH(uint32_t n)
{
	m_sh2_state->r[n] -= 4;
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->mach);
}

/*  STS.L   MACL,@-Rn */
void StandaloneSh2::STSMMACL(uint32_t n)
{
	m_sh2_state->r[n] -= 4;
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->macl);
}

/*  STS.L   PR,@-Rn */
void StandaloneSh2::STSMPR(uint32_t n)
{
	m_sh2_state->r[n] -= 4;
	m_sh2_state->ea = m_sh2_state->r[n];
	write_long(m_sh2_state->ea, m_sh2_state->pr);
}

/*  SUB     Rm,Rn */
void StandaloneSh2::SUB(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] -= m_sh2_state->r[m];
}

/*  SUBC    Rm,Rn */
void StandaloneSh2::SUBC(uint32_t m, uint32_t n)
{
	uint32_t tmp1 = m_sh2_state->r[n] - m_sh2_state->r[m];
	uint32_t tmp0 = m_sh2_state->r[n];
	m_sh2_state->r[n] = tmp1 - (m_sh2_state->sr & SH_T);
	if (tmp0 < tmp1)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
	if (tmp1 < m_sh2_state->r[n])
		m_sh2_state->sr |= SH_T;
}

/*  SUBV    Rm,Rn */
void StandaloneSh2::SUBV(uint32_t m, uint32_t n)
{
	int32_t dest = BIT(m_sh2_state->r[n], 31);
	int32_t src = BIT(m_sh2_state->r[m], 31);
	src += dest;

	m_sh2_state->r[n] -= m_sh2_state->r[m];

	int32_t ans = BIT(m_sh2_state->r[n], 31);
	ans += dest;

	if (src == 1 && ans == 1)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  SWAP.B  Rm,Rn */
void StandaloneSh2::SWAPB(uint32_t m, uint32_t n)
{
	uint32_t temp = m_sh2_state->r[m] & 0xffff0000;
	temp |= (m_sh2_state->r[m] & 0x000000ff) << 8;
	m_sh2_state->r[n] = (uint8_t)(m_sh2_state->r[m] >> 8);
	m_sh2_state->r[n] = m_sh2_state->r[n] | temp;
}

/*  SWAP.W  Rm,Rn */
void StandaloneSh2::SWAPW(uint32_t m, uint32_t n)
{
	uint32_t temp = m_sh2_state->r[m] >> 16;
	m_sh2_state->r[n] = (m_sh2_state->r[m] << 16) | temp;
}

/*  TAS.B   @Rn */
void StandaloneSh2::TAS(uint32_t n)
{
	m_sh2_state->ea = m_sh2_state->r[n];

	/* Bus Lock enable */
	uint32_t temp = read_byte(m_sh2_state->ea);
	if (temp == 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
	temp |= 0x80;
	/* Bus Lock disable */
	write_byte(m_sh2_state->ea, temp);
	m_sh2_state->icount -= 3;
}

/*  TST     Rm,Rn */
void StandaloneSh2::TST(uint32_t m, uint32_t n)
{
	if ((m_sh2_state->r[n] & m_sh2_state->r[m]) == 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  TST     #imm,R0 */
void StandaloneSh2::TSTI(uint32_t i)
{
	uint32_t imm = i & 0xff;

	if ((imm & m_sh2_state->r[0]) == 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
}

/*  TST.B   #imm,@(R0,GBR) */
void StandaloneSh2::TSTM(uint32_t i)
{
	uint32_t imm = i & 0xff;

	m_sh2_state->ea = m_sh2_state->gbr + m_sh2_state->r[0];
	if ((imm & read_byte(m_sh2_state->ea)) == 0)
		m_sh2_state->sr |= SH_T;
	else
		m_sh2_state->sr &= ~SH_T;
	m_sh2_state->icount -= 2;
}

/*  XOR     Rm,Rn */
void StandaloneSh2::XOR(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] ^= m_sh2_state->r[m];
}

/*  XOR     #imm,R0 */
void StandaloneSh2::XORI(uint32_t i)
{
	m_sh2_state->r[0] ^= i & 0x000000ff;
}

/*  XOR.B   #imm,@(R0,GBR) */
void StandaloneSh2::XORM(uint32_t i)
{
	m_sh2_state->ea = m_sh2_state->gbr + m_sh2_state->r[0];
	write_byte(m_sh2_state->ea, read_byte(m_sh2_state->ea) ^ (uint8_t)i);
	m_sh2_state->icount -= 2;
}

/*  XTRCT   Rm,Rn */
void StandaloneSh2::XTRCT(uint32_t m, uint32_t n)
{
	m_sh2_state->r[n] = (m_sh2_state->r[n] >> 16) | (m_sh2_state->r[m] << 16);
}

/*  SLEEP */
void StandaloneSh2::SLEEP()
{
	/* 0 = normal mode */
	/* 1 = enters into power-down mode */
	/* 2 = go out the power-down mode after an exception */
	if (m_sh2_state->sleep_mode != 2)
		m_sh2_state->pc -= 2;
	m_sh2_state->icount -= 2;
	/* Wait_for_exception; */
	if (m_sh2_state->sleep_mode == 0)
		m_sh2_state->sleep_mode = 1;
	else if (m_sh2_state->sleep_mode == 2)
		m_sh2_state->sleep_mode = 0;
}

/* Common dispatch */

void StandaloneSh2::op0010(uint16_t opcode)
{
	switch (opcode & 15)
	{
	case  0: MOVBS(REG_M, REG_N);   break;
	case  1: MOVWS(REG_M, REG_N);   break;
	case  2: MOVLS(REG_M, REG_N);   break;
	case  3: ILLEGAL();             break;
	case  4: MOVBM(REG_M, REG_N);   break;
	case  5: MOVWM(REG_M, REG_N);   break;
	case  6: MOVLM(REG_M, REG_N);   break;
	case  7: DIV0S(REG_M, REG_N);   break;
	case  8: TST(REG_M, REG_N);     break;
	case  9: AND(REG_M, REG_N);     break;
	case 10: XOR(REG_M, REG_N);     break;
	case 11: OR(REG_M, REG_N);      break;
	case 12: CMPSTR(REG_M, REG_N);  break;
	case 13: XTRCT(REG_M, REG_N);   break;
	case 14: MULU(REG_M, REG_N);    break;
	case 15: MULS(REG_M, REG_N);    break;
	}
}

void StandaloneSh2::op0011(uint16_t opcode)
{
	switch (opcode & 15)
	{
	case  0: CMPEQ(REG_M, REG_N);   break;
	case  1: ILLEGAL();             break;
	case  2: CMPHS(REG_M, REG_N);   break;
	case  3: CMPGE(REG_M, REG_N);   break;
	case  4: DIV1(REG_M, REG_N);    break;
	case  5: DMULU(REG_M, REG_N);   break;
	case  6: CMPHI(REG_M, REG_N);   break;
	case  7: CMPGT(REG_M, REG_N);   break;
	case  8: SUB(REG_M, REG_N);     break;
	case  9: ILLEGAL();             break;
	case 10: SUBC(REG_M, REG_N);    break;
	case 11: SUBV(REG_M, REG_N);    break;
	case 12: ADD(REG_M, REG_N);     break;
	case 13: DMULS(REG_M, REG_N);   break;
	case 14: ADDC(REG_M, REG_N);    break;
	case 15: ADDV(REG_M, REG_N);    break;
	}
}

void StandaloneSh2::op0110(uint16_t opcode)
{
	switch (opcode & 15)
	{
	case  0: MOVBL(REG_M, REG_N);   break;
	case  1: MOVWL(REG_M, REG_N);   break;
	case  2: MOVLL(REG_M, REG_N);   break;
	case  3: MOV(REG_M, REG_N);     break;
	case  4: MOVBP(REG_M, REG_N);   break;
	case  5: MOVWP(REG_M, REG_N);   break;
	case  6: MOVLP(REG_M, REG_N);   break;
	case  7: NOT(REG_M, REG_N);     break;
	case  8: SWAPB(REG_M, REG_N);   break;
	case  9: SWAPW(REG_M, REG_N);   break;
	case 10: NEGC(REG_M, REG_N);    break;
	case 11: NEG(REG_M, REG_N);     break;
	case 12: EXTUB(REG_M, REG_N);   break;
	case 13: EXTUW(REG_M, REG_N);   break;
	case 14: EXTSB(REG_M, REG_N);   break;
	case 15: EXTSW(REG_M, REG_N);   break;
	}
}

void StandaloneSh2::op1000(uint16_t opcode)
{
	switch ((opcode >> 8) & 15)
	{
	case  0: MOVBS4(opcode & 0x0f, REG_M);  break;
	case  1: MOVWS4(opcode & 0x0f, REG_M);  break;
	case  2: ILLEGAL();                     break;
	case  3: ILLEGAL();                     break;
	case  4: MOVBL4(REG_M, opcode & 0x0f);  break;
	case  5: MOVWL4(REG_M, opcode & 0x0f);  break;
	case  6: ILLEGAL();                     break;
	case  7: ILLEGAL();                     break;
	case  8: CMPIM(opcode & 0xff);          break;
	case  9: BT(opcode & 0xff);             break;
	case 10: ILLEGAL();                     break;
	case 11: BF(opcode & 0xff);             break;
	case 12: ILLEGAL();                     break;
	case 13: BTS(opcode & 0xff);            break;
	case 14: ILLEGAL();                     break;
	case 15: BFS(opcode & 0xff);            break;
	}
}


void StandaloneSh2::op1100(uint16_t opcode)
{
	switch ((opcode >> 8) & 15)
	{
	case  0: MOVBSG(opcode & 0xff);     break;
	case  1: MOVWSG(opcode & 0xff);     break;
	case  2: MOVLSG(opcode & 0xff);     break;
	case  3: TRAPA(opcode & 0xff);      break; // sh2/4 differ
	case  4: MOVBLG(opcode & 0xff);     break;
	case  5: MOVWLG(opcode & 0xff);     break;
	case  6: MOVLLG(opcode & 0xff);     break;
	case  7: MOVA(opcode & 0xff);       break;
	case  8: TSTI(opcode & 0xff);       break;
	case  9: ANDI(opcode & 0xff);       break;
	case 10: XORI(opcode & 0xff);       break;
	case 11: ORI(opcode & 0xff);        break;
	case 12: TSTM(opcode & 0xff);       break;
	case 13: ANDM(opcode & 0xff);       break;
	case 14: XORM(opcode & 0xff);       break;
	case 15: ORM(opcode & 0xff);        break;
	}
}

// SH4 cases fall through to here too
void StandaloneSh2::execute_one_0000(uint16_t opcode)
{
	// 04,05,06,07 always the same, 0c,0d,0e,0f always the same, other change based on upper bits

	switch (opcode & 0x3f)
	{
	case 0x00: ILLEGAL();               break;
	case 0x01: ILLEGAL();               break;
	case 0x02: STCSR(REG_N);            break;
	case 0x03: BSRF(REG_N);             break;
	case 0x04: MOVBS0(REG_M, REG_N);    break;
	case 0x05: MOVWS0(REG_M, REG_N);    break;
	case 0x06: MOVLS0(REG_M, REG_N);    break;
	case 0x07: MULL(REG_M, REG_N);      break;
	case 0x08: CLRT();                  break;
	case 0x09: NOP();                   break;
	case 0x0a: STSMACH(REG_N);          break;
	case 0x0b: RTS();                   break;
	case 0x0c: MOVBL0(REG_M, REG_N);    break;
	case 0x0d: MOVWL0(REG_M, REG_N);    break;
	case 0x0e: MOVLL0(REG_M, REG_N);    break;
	case 0x0f: MAC_L(REG_M, REG_N);     break;

	case 0x10: ILLEGAL();               break;
	case 0x11: ILLEGAL();               break;
	case 0x12: STCGBR(REG_N);           break;
	case 0x13: ILLEGAL();               break;
	case 0x14: MOVBS0(REG_M, REG_N);    break;
	case 0x15: MOVWS0(REG_M, REG_N);    break;
	case 0x16: MOVLS0(REG_M, REG_N);    break;
	case 0x17: MULL(REG_M, REG_N);      break;
	case 0x18: SETT();                  break;
	case 0x19: DIV0U();                 break;
	case 0x1a: STSMACL(REG_N);          break;
	case 0x1b: SLEEP();                 break;
	case 0x1c: MOVBL0(REG_M, REG_N);    break;
	case 0x1d: MOVWL0(REG_M, REG_N);    break;
	case 0x1e: MOVLL0(REG_M, REG_N);    break;
	case 0x1f: MAC_L(REG_M, REG_N);     break;

	case 0x20: ILLEGAL();               break;
	case 0x21: ILLEGAL();               break;
	case 0x22: STCVBR(REG_N);           break;
	case 0x23: BRAF(REG_N);             break;
	case 0x24: MOVBS0(REG_M, REG_N);    break;
	case 0x25: MOVWS0(REG_M, REG_N);    break;
	case 0x26: MOVLS0(REG_M, REG_N);    break;
	case 0x27: MULL(REG_M, REG_N);      break;
	case 0x28: CLRMAC();                break;
	case 0x29: MOVT(REG_N);             break;
	case 0x2a: STSPR(REG_N);            break;
	case 0x2b: RTE();                   break;
	case 0x2c: MOVBL0(REG_M, REG_N);    break;
	case 0x2d: MOVWL0(REG_M, REG_N);    break;
	case 0x2e: MOVLL0(REG_M, REG_N);    break;
	case 0x2f: MAC_L(REG_M, REG_N);     break;

	case 0x30: ILLEGAL();               break;
	case 0x31: ILLEGAL();               break;
	case 0x32: ILLEGAL();               break;
	case 0x33: ILLEGAL();               break;
	case 0x34: MOVBS0(REG_M, REG_N);    break;
	case 0x35: MOVWS0(REG_M, REG_N);    break;
	case 0x36: MOVLS0(REG_M, REG_N);    break;
	case 0x37: MULL(REG_M, REG_N);      break;
	case 0x38: ILLEGAL();               break;
	case 0x39: ILLEGAL();               break;
	case 0x3a: ILLEGAL();               break;
	case 0x3b: ILLEGAL();               break;
	case 0x3c: MOVBL0(REG_M, REG_N);    break;
	case 0x3d: MOVWL0(REG_M, REG_N);    break;
	case 0x3e: MOVLL0(REG_M, REG_N);    break;
	case 0x3f: MAC_L(REG_M, REG_N);     break;
	}
}

// SH4 cases fall through to here too
void StandaloneSh2::execute_one_4000(uint16_t opcode)
{
	// 0f always the same, others differ

	switch (opcode & 0x3f)
	{
	case 0x00: SHLL(REG_N);         break;
	case 0x01: SHLR(REG_N);         break;
	case 0x02: STSMMACH(REG_N);     break;
	case 0x03: STCMSR(REG_N);       break;
	case 0x04: ROTL(REG_N);         break;
	case 0x05: ROTR(REG_N);         break;
	case 0x06: LDSMMACH(REG_N);     break;
	case 0x07: LDCMSR(opcode);      break;
	case 0x08: SHLL2(REG_N);        break;
	case 0x09: SHLR2(REG_N);        break;
	case 0x0a: LDSMACH(REG_N);      break;
	case 0x0b: JSR(REG_N);          break;
	case 0x0c: ILLEGAL();           break;
	case 0x0d: ILLEGAL();           break;
	case 0x0e: LDCSR(opcode);       break;
	case 0x0f: MAC_W(REG_M, REG_N); break;

	case 0x10: DT(REG_N);           break;
	case 0x11: CMPPZ(REG_N);        break;
	case 0x12: STSMMACL(REG_N);     break;
	case 0x13: STCMGBR(REG_N);      break;
	case 0x14: ILLEGAL();           break;
	case 0x15: CMPPL(REG_N);        break;
	case 0x16: LDSMMACL(REG_N);     break;
	case 0x17: LDCMGBR(REG_N);      break;
	case 0x18: SHLL8(REG_N);        break;
	case 0x19: SHLR8(REG_N);        break;
	case 0x1a: LDSMACL(REG_N);      break;
	case 0x1b: TAS(REG_N);          break;
	case 0x1c: ILLEGAL();           break;
	case 0x1d: ILLEGAL();           break;
	case 0x1e: LDCGBR(REG_N);       break;
	case 0x1f: MAC_W(REG_M, REG_N); break;

	case 0x20: SHAL(REG_N);         break;
	case 0x21: SHAR(REG_N);         break;
	case 0x22: STSMPR(REG_N);       break;
	case 0x23: STCMVBR(REG_N);      break;
	case 0x24: ROTCL(REG_N);        break;
	case 0x25: ROTCR(REG_N);        break;
	case 0x26: LDSMPR(REG_N);       break;
	case 0x27: LDCMVBR(REG_N);      break;
	case 0x28: SHLL16(REG_N);       break;
	case 0x29: SHLR16(REG_N);       break;
	case 0x2a: LDSPR(REG_N);        break;
	case 0x2b: JMP(REG_N);          break;
	case 0x2c: ILLEGAL();           break;
	case 0x2d: ILLEGAL();           break;
	case 0x2e: LDCVBR(REG_N);       break;
	case 0x2f: MAC_W(REG_M, REG_N); break;

	case 0x30: ILLEGAL();           break;
	case 0x31: ILLEGAL();           break;
	case 0x32: ILLEGAL();           break;
	case 0x33: ILLEGAL();           break;
	case 0x34: ILLEGAL();           break;
	case 0x35: ILLEGAL();           break;
	case 0x36: ILLEGAL();           break;
	case 0x37: ILLEGAL();           break;
	case 0x38: ILLEGAL();           break;
	case 0x39: ILLEGAL();           break;
	case 0x3a: ILLEGAL();           break;
	case 0x3b: ILLEGAL();           break;
	case 0x3c: ILLEGAL();           break;
	case 0x3d: ILLEGAL();           break;
	case 0x3e: ILLEGAL();           break;
	case 0x3f: MAC_W(REG_M, REG_N); break;

	}
}

void StandaloneSh2::execute_one(const uint16_t opcode)
{
	switch ((opcode >> 12) & 15)
	{
		case  0: execute_one_0000(opcode);              break;
		case  1: MOVLS4(REG_M, opcode & 0xf, REG_N);    break;
		case  2: op0010(opcode);                        break;
		case  3: op0011(opcode);                        break;
		case  4: execute_one_4000(opcode);              break;
		case  5: MOVLL4(REG_M, opcode & 0x0f, REG_N);   break;
		case  6: op0110(opcode);                        break;
		case  7: ADDI(opcode & 0xff, REG_N);            break;
		case  8: op1000(opcode);                        break;
		case  9: MOVWI(opcode & 0xff, REG_N);           break;
		case 10: BRA(opcode & 0xfff);                   break;
		case 11: BSR(opcode & 0xfff);                   break;
		case 12: op1100(opcode);                        break;
		case 13: MOVLI(opcode & 0xff, REG_N);           break;
		case 14: MOVI(opcode & 0xff, REG_N);            break;
		case 15: execute_one_f000(opcode);              break;
	}
}


void StandaloneSh2::LDCMSR(const uint16_t opcode) // passes Rn
{
	const uint32_t rn = REG_N;
	m_sh2_state->ea = m_sh2_state->r[rn];
	m_sh2_state->sr = read_long(m_sh2_state->ea) & SH_FLAGS;
	m_sh2_state->r[rn] += 4;
	m_sh2_state->icount -= 2;
	m_test_irq = 1;
}

/*  LDC     Rm,SR */
void StandaloneSh2::LDCSR(const uint16_t opcode) // passes Rn
{
	m_sh2_state->sr = m_sh2_state->r[REG_N] & SH_FLAGS;
	m_test_irq = 1;
}

/*  RTE */
void StandaloneSh2::RTE()
{
	m_sh2_state->ea = m_sh2_state->r[15];
	m_sh2_state->m_delay = read_long(m_sh2_state->ea);
	m_sh2_state->r[15] += 4;
	m_sh2_state->ea = m_sh2_state->r[15];
	m_sh2_state->sr = read_long(m_sh2_state->ea) & SH_FLAGS;
	m_sh2_state->r[15] += 4;
	m_sh2_state->icount -= 3;
	m_test_irq = 1;
}

/*  TRAPA   #imm */
void StandaloneSh2::TRAPA(uint32_t i)
{
	uint32_t imm = i & 0xff;


	m_sh2_state->ea = m_sh2_state->vbr + imm * 4;

	m_sh2_state->r[15] -= 4;
	write_long(m_sh2_state->r[15], m_sh2_state->sr);
	m_sh2_state->r[15] -= 4;
	write_long(m_sh2_state->r[15], m_sh2_state->pc);

	m_sh2_state->pc = read_long(m_sh2_state->ea);

	m_sh2_state->icount -= 7;
}

/*  ILLEGAL */
void StandaloneSh2::ILLEGAL()
{
    s.illegal_pc = s.pc - 2; s.illegal_opcode = read_word(s.illegal_pc);
	//logerror("Illegal opcode at %08x\n", m_sh2_state->pc - 2);


	m_sh2_state->r[15] -= 4;
	write_long(m_sh2_state->r[15], m_sh2_state->sr);     /* push SR onto stack */
	m_sh2_state->r[15] -= 4;
	write_long(m_sh2_state->r[15], m_sh2_state->pc - 2); /* push PC onto stack */

	/* fetch PC */
	m_sh2_state->pc = read_long(m_sh2_state->vbr + 4 * 4);

	/* TODO: timing is a guess */
	m_sh2_state->icount -= 5;
}

void StandaloneSh2::execute_one_f000(uint16_t opcode)
{
	ILLEGAL();
}
