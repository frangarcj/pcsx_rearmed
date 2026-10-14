/* Source MAME CR-589 command bodies, BSD-3-Clause, smf. */
static constexpr int identity_offset = 0x3ab;
static constexpr char download_identity[] = "MATSHITA CD98Q4 DOWNLOADGS0N";
void matsushita_cr589_device::ExecCommand()
{
	switch( command[0] )
	{
	case T10SPC_CMD_INQUIRY:
		atapi_source_log(this, "T10MMC: INQUIRY\n");
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT8( &command[4] );
		break;

	case 0x3b: // WRITE BUFFER
		bufferOffset = get_u24be( &command[3] );
		if (!valid_buffer_range(bufferOffset, get_u24be(&command[6]))) return;
		m_phase = SCSI_PHASE_DATAOUT;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = get_u24be( &command[6] );
		break;

	case 0x3c: // READ BUFFER
		bufferOffset = get_u24be( &command[3] );
		if (!valid_buffer_range(bufferOffset, get_u24be(&command[6]))) return;
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = get_u24be( &command[6] );
		break;

	case 0xcc: // FIRMWARE DOWNLOAD ENABLE
		m_phase = SCSI_PHASE_DATAOUT;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT16( &command[7] );
		break;

	default:
		t10mmc::ExecCommand();
		break;
	}
}

void matsushita_cr589_device::ReadData( uint8_t *data, int dataLength )
{
	switch( command[0] )
	{
	case T10SPC_CMD_INQUIRY:
		memset(data, 0, dataLength);

		safe_mmc_read(data, dataLength);

		if( download )
		{
			memcpy( &data[8], download_identity, 28 );
		}
		else
		{
			memcpy( &data[8], &buffer[identity_offset], 28 );
		}
		break;

	case 0x3c: // READ BUFFER
		memcpy( data, &buffer[bufferOffset], dataLength );
		bufferOffset += dataLength;
		break;

	default:
		safe_mmc_read(data, dataLength);
		break;
	}
}

void matsushita_cr589_device::WriteData( uint8_t *data, int dataLength )
{
	switch( command[0] )
	{
		case 0x3b: // WRITE BUFFER
			if (dataLength < 32 || !valid_buffer_range(bufferOffset, dataLength)) return;
			memcpy( &buffer[bufferOffset], data + 32, dataLength - 32 );
			bufferOffset += dataLength;
			break;

		case 0xcc: // FIRMWARE DOWNLOAD ENABLE
			if (dataLength < 28) { malformed_buffer(); return; }
			if( memcmp( data, &buffer[identity_offset], 28 ) == 0 )
			{
				download = 1;
			}
			else if( memcmp( data, download_identity, 28 ) == 0 )
			{
				download = 0;
			}
			break;

		default:
			safe_mmc_write(data, dataLength);
			break;
	}
}

void matsushita_cr589_device::device_start()
{
	
	
	

	atapi_hle_device::device_start();

	/// TODO: split identify buffer into another method as device_start() should be called after it's filled in, but the atapi_cdrom_device has it's own.
	memset(m_identify_buffer, 0, sizeof(m_identify_buffer));

	m_identify_buffer[0] = 0x8500; // ATAPI device, cmd set 5 compliant, DRQ within 3 ms of PACKET command

	m_identify_buffer[23] = ('1' << 8) | '.';
	m_identify_buffer[24] = ('0' << 8) | ' ';
	m_identify_buffer[25] = (' ' << 8) | ' ';
	m_identify_buffer[26] = (' ' << 8) | ' ';

	m_identify_buffer[27] = ('M' << 8) | 'A';
	m_identify_buffer[28] = ('T' << 8) | 'S';
	m_identify_buffer[29] = ('H' << 8) | 'I';
	m_identify_buffer[30] = ('T' << 8) | 'A';
	m_identify_buffer[31] = (' ' << 8) | 'C';
	m_identify_buffer[32] = ('R' << 8) | '-';
	m_identify_buffer[33] = ('5' << 8) | '8';
	m_identify_buffer[34] = ('9' << 8) | ' ';
	m_identify_buffer[35] = (' ' << 8) | ' ';
	m_identify_buffer[36] = (' ' << 8) | ' ';
	m_identify_buffer[37] = (' ' << 8) | ' ';
	m_identify_buffer[38] = (' ' << 8) | ' ';
	m_identify_buffer[39] = (' ' << 8) | ' ';
	m_identify_buffer[40] = (' ' << 8) | ' ';
	m_identify_buffer[41] = (' ' << 8) | ' ';
	m_identify_buffer[42] = (' ' << 8) | ' ';
	m_identify_buffer[43] = (' ' << 8) | ' ';
	m_identify_buffer[44] = (' ' << 8) | ' ';
	m_identify_buffer[45] = (' ' << 8) | ' ';
	m_identify_buffer[46] = (' ' << 8) | ' ';

	m_identify_buffer[49] = 0x0400; // IORDY may be disabled
}

