#pragma once
#include <stdint.h>
#include <LibC/stdbool.h>
#include <Disk/GenericDiskController.h>

#define IDEATA 1
#define IDEATAPI 2
#define IDEUNKNOWN 3
#define SATA_AHCI 4
#define SATAPI_AHCI 5
#define NVMe 6
#define USB 7

class StorageDevice {

public:
    StorageDevice(GenericDiskController* disk_controller,uint32_t port);
    ~StorageDevice();
    virtual void read(uint32_t lbal,uint32_t lbah,uint32_t bytesOffset,uint16_t* buf,uint16_t bytesCount);
    virtual void read_to_cache(uint32_t lbal,uint32_t lbah,uint32_t bytesOffset);
    virtual uint16_t get_hardware_protocol();
    uint16_t get_sector_size();
    uint32_t get_low_dword_cache();
    uint32_t get_high_dword_cache();
    uint32_t get_port();
protected:
    uint8_t transfer_mode;
    uint8_t command_set;
    uint8_t* data_buffer;
    uint16_t data_buffer_size;
    void set_sector_size(uint16_t sector_size);
    uint16_t sector_size;
    uint32_t cached_data[2];
    GenericDiskController* disk_controller;
    uint32_t port;
private:
    void small_read(uint32_t lbal,uint32_t lbah,uint32_t bytesOffset,uint16_t* buf,uint16_t bytesCount);
};