#include <Disk/AHCI/AHCIController.h>
#include <LibC/stdstring.h>
#include <Memory/malloc.h>
#define BYTES_PER_PRDT 8192

AHCIController::AHCIController(PCI::Device& device,PCI::Access& access) : GenericDiskController(device,access)
{
	this->initialize(device,access);
}

uint16_t AHCIController::get_controller_type()
{
	return AHCIStorageController;
}
void AHCIController::initialize(PCI::Device& device,PCI::Access& access)
{
    
    this->m_hba = (AHCI::HBA_MEM*)(
                        PCI::read(access,
                        device.get_segment(),
                        device.get_bus(),
                        device.get_device_number(),
                        device.get_function_number(),
                        AHCI_ABAR_BASE) | 
                        (PCI::read(access,
                        device.get_segment(),
                        device.get_bus(),
                        device.get_device_number(),
                        device.get_function_number(),
                        AHCI_ABAR_BASE+2) << 16)
                    );
	this->default_transfer_mode = DMATransferMode;
}
bool AHCIController::probe_port_connected(uint32_t port)
{
    if(port > 31)
        return false;
	else
		return (((get_hba().pi) >> port) & 0x1) == 1;
}
bool AHCIController::read(__attribute__((unused)) uint8_t transfer_mode,uint8_t commandset,uint32_t port_number,uint32_t lbal,uint32_t lbah,uint16_t* buf,uint16_t sectors_count,uint16_t sector_size)
{
	if(commandset == ATACommandSet)
	{
		return this->read_ata(port_number,lbal,lbah,buf,sectors_count,sector_size);
	}
	return false;
}	

bool AHCIController::read_ata(uint8_t port_number,uint32_t lbal,uint32_t lbah,uint16_t* buf,uint16_t sectors_count,uint16_t sector_size)
{
    AHCI::HBA_PORT& port = get_port(port_number);
    port.is = (uint32_t) -1;
	int spin = 0; // Spin lock timeout counter
	int slot = this->find_freeslot(port);
	if (slot == -1)
		return false;
 
	AHCI::HBA_CMD_HEADER& cmdheader = get_cmd_header(port,slot);
	cmdheader.command = sizeof(AHCI::FIS_REG_H2D)/sizeof(uint32_t);	// Command FIS size
	cmdheader.prdtl = (uint16_t)((sectors_count-1)>>4) + 1;	// PRDT entries count
 
	AHCI::HBA_CMD_TBL& cmdtbl = get_cmd_table(cmdheader);
	int i=0;
	// 8K bytes (16 sectors of 512 byte sized sectors!) per PRDT
	for (i=0; i<cmdheader.prdtl-1; i++)
	{
		cmdtbl.prdt_entry[i].dba = (uint32_t) buf;
		cmdtbl.prdt_entry[i].info = ((BYTES_PER_PRDT-1) & (0x3FFFFF)) | (1<<31);	// 8K bytes (this value should always be set to 1 less than the actual value)
		buf += (BYTES_PER_PRDT/sizeof(uint16_t));	// 4K words
		sectors_count -= (BYTES_PER_PRDT / sector_size);
	}
	// Last entry
	cmdtbl.prdt_entry[i].dba = (uint32_t) buf;
	cmdtbl.prdt_entry[i].info = (((sectors_count*sector_size)-1) & (0x3FFFFF)) | (1<<31);

	// Setup command
	AHCI::FIS_REG_H2D *cmdfis = (AHCI::FIS_REG_H2D*)(cmdtbl.cfis);
	memset((uint8_t*)cmdfis,0,sizeof(AHCI::FIS_REG_H2D));
	cmdfis->h.fis_type = AHCI::FIS_TYPE_REG_H2D;
	cmdfis->h.attrs = 0x80;	// Command
	cmdfis->command = ATA_CMD_READ_DMA_EXT;
 
	cmdfis->lba0 = (uint8_t)lbal;
	cmdfis->lba1 = (uint8_t)(lbal>>8);
	cmdfis->lba2 = (uint8_t)(lbal>>16);
	cmdfis->device = 1<<6;	// LBA mode
 
	cmdfis->lba3 = (uint8_t)(lbal>>24);
	cmdfis->lba4 = (uint8_t)lbah;
	cmdfis->lba5 = (uint8_t)(lbah>>8);
 
	cmdfis->countl = sectors_count & 0xFF;
	cmdfis->counth = (sectors_count >> 8) & 0xFF;
 
	// The below loop waits until the port is no longer busy before issuing a new command
	while ((port.tfd & (ATA_SR_BSY | ATA_SR_DRQ)) && spin < 1000000)
	{
		spin++;
		if (spin == 1000000)
		{
			return false;
		}
	}
	
	port.ci = 1<<slot;	// Issue command
 
	// Wait for completion
	while (1)
	{
		// In some longer duration reads, it may be helpful to spin on the DPS bit 
		// in the PxIS port field as well (1 << 5)
		if ((port.ci & (1<<slot)) == 0) 
			break;
		if (port.is & AHCI_HBA_PxIS_TFES)	// Task file error
		{
			break;
		}
	}
 
	// Check again
	if (port.is & AHCI_HBA_PxIS_TFES)
	{
		return false;
	}
	return true;
}
AHCI::HBA_PORT& AHCIController::get_port(uint8_t port)
{
    return get_hba().ports[port];
}
int AHCIController::find_freeslot(AHCI::HBA_PORT& port)
{
    uint32_t slots = (port.sact | port.ci);
	for (int i=0; i < AHCI_MAXIMUM_COMMAND_SLOTS; i++)
	{
		if ((slots&1) == 0)
			return i;
		slots >>= 1;
	}
    return -1;
}
uint16_t AHCIController::get_logical_sector_size(uint32_t port)
{
	AHCI::HBA_PORT& port_ptr = this->get_port(port);
	if(port_ptr.sig != 0x00000101)
	{
		return 0xffff;
	}
	this->ata_identify(port,(uint16_t*)&this->m_cached_identify_data);
    if((this->m_cached_identify_data.physical_logical_sector & (1 << 12)) == 0)
        return ATA_LOGICAL_SECTOR_SIZE;
    if(this->m_cached_identify_data.logical_sector_size[0] == 0)
        return ATA_LOGICAL_SECTOR_SIZE;
    else
        return this->m_cached_identify_data.logical_sector_size[0] << 1;
}

bool AHCIController::ata_identify(uint8_t port_number,uint16_t* buf)
{
	AHCI::HBA_PORT& port = get_port(port_number);

    port.is = (uint32_t) -1;
	int spin = 0; // Spin lock timeout counter
	int slot = this->find_freeslot(port);
	if (slot == -1)
		return false;
 
	AHCI::HBA_CMD_HEADER& cmdheader = get_cmd_header(port,slot);
	cmdheader.command = sizeof(AHCI::FIS_REG_H2D)/sizeof(uint32_t); // Command FIS size
	cmdheader.prdtl = 1; // PRDT entries count

	AHCI::HBA_CMD_TBL& cmdtbl = get_cmd_table(cmdheader);
	cmdtbl.prdt_entry[0].dba = (uint32_t)buf;
    cmdtbl.prdt_entry[0].info = ATA_LOGICAL_SECTOR_SIZE-1;
 
	// Setup command
	AHCI::FIS_REG_H2D *cmdfis = (AHCI::FIS_REG_H2D*)(cmdtbl.cfis);
	memset((uint8_t*)cmdfis,0,sizeof(AHCI::FIS_REG_H2D));
	cmdfis->h.fis_type = AHCI::FIS_TYPE_REG_H2D;
	cmdfis->command = ATA_CMD_IDENTIFY;
	cmdfis->device = 0;
	cmdfis->h.attrs |= (1 << 7);
 
	// The below loop waits until the port is no longer busy before issuing a new command
	while ((port.tfd & (ATA_SR_BSY | ATA_SR_DRQ)) && spin < 1000000)
	{
		spin++;
		if (spin == 1000000)
		{
			return false;
		}
	}

	port.cmd |= (1 << 4) | (1 << 0);
	port.ci = 1<<slot;	// Issue command
 
	// Wait for completion
	while (1)
	{
		// In some longer duration reads, it may be helpful to spin on the DPS bit 
		// in the PxIS port field as well (1 << 5)
		if ((port.ci & (1<<slot)) == 0) 
			break;
		if (port.is & AHCI_HBA_PxIS_TFES)	// Task file error
		{
			break;
		}
	}
 
	// Check again
	if (port.is & AHCI_HBA_PxIS_TFES)
	{
		return false;
	}
	return true;
}
bool AHCIController::atapi_identify(uint8_t port_number,uint16_t* buf)
{
	/* TODO: Fix ATAPI IDENTIFY to actually work */
	AHCI::HBA_PORT& port = get_port(port_number);

    port.is = (uint32_t) -1;
	int spin = 0; // Spin lock timeout counter
	int slot = this->find_freeslot(port);
	if (slot == -1)
		return false;
 
	AHCI::HBA_CMD_HEADER& cmdheader = get_cmd_header(port,slot);
	cmdheader.command = sizeof(AHCI::FIS_REG_H2D)/sizeof(uint32_t); // Command FIS size
	cmdheader.prdtl = 1; // PRDT entries count


	AHCI::HBA_CMD_TBL& cmdtbl = get_cmd_table(cmdheader);
	cmdtbl.prdt_entry[0].dba = (uint32_t)buf;
    cmdtbl.prdt_entry[0].info = ATA_LOGICAL_SECTOR_SIZE-1;
 
	// Setup command
	AHCI::FIS_REG_H2D *cmdfis = (AHCI::FIS_REG_H2D*)(cmdtbl.cfis);
	memset((uint8_t*)cmdfis,0,sizeof(AHCI::FIS_REG_H2D));
	cmdfis->h.fis_type = AHCI::FIS_TYPE_REG_H2D;
	cmdfis->command = ATA_CMD_IDENTIFY;
	cmdfis->device = 0;
	cmdfis->h.attrs |= (1 << 7);
 
	// The below loop waits until the port is no longer busy before issuing a new command
	while ((port.tfd & (ATA_SR_BSY | ATA_SR_DRQ)) && spin < 1000000)
	{
		spin++;
		if (spin == 1000000)
		{
			return false;
		}
	}

	port.cmd |= (1 << 4) | (1 << 0);
	port.ci = 1<<slot;	// Issue command
 
	// Wait for completion
	while (1)
	{
		// In some longer duration reads, it may be helpful to spin on the DPS bit 
		// in the PxIS port field as well (1 << 5)
		if ((port.ci & (1<<slot)) == 0) 
			break;
		if (port.is & AHCI_HBA_PxIS_TFES)	// Task file error
		{
			break;
		}
	}
 
	// Check again
	if (port.is & AHCI_HBA_PxIS_TFES)
	{
		return false;
	}
	return true;
}
AHCI::HBA_MEM& AHCIController::get_hba()
{
	return *this->m_hba;
}
AHCI::HBA_CMD_HEADER& AHCIController::get_cmd_header(AHCI::HBA_PORT& port,int slot)
{
	AHCI::HBA_CMD_HEADER* cmd_header = (AHCI::HBA_CMD_HEADER*)port.clb;
	cmd_header += slot;
	return (AHCI::HBA_CMD_HEADER&)*cmd_header;
}
AHCI::HBA_CMD_TBL& AHCIController::get_cmd_table(AHCI::HBA_CMD_HEADER& cmd_header)
{
	AHCI::HBA_CMD_TBL* cmd_tbl = (AHCI::HBA_CMD_TBL*)cmd_header.ctba;
	return *cmd_tbl;
}