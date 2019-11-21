#include <Filesystem/LinuxFilesystems/Ext2Filesystem.h>
#include <Memory/malloc.h>
#include <LibC/stdstring.h>

/*
Ext2Filesystem::Ext2Filesystem(GenericStoragePartition* partition)
{
    this->initialize(partition);
    this->filesystem_type = FilesystemType::Ext2FS;
}
*/
void Ext2Filesystem::initialize(GenericStoragePartition& partition)
{
    this->partition = &partition;
    this->partition->read(0,0,Ext2FS_Superblock_Offset,(uint16_t*)this->m_cached_superblock,512);
    this->m_cached_inode = (char*)cache_calloc(this->get_inode_size());
    
    this->m_cached_data_block = (char*)cache_calloc(this->get_block_size());
    this->m_cached_data_block_number = 0xFFFFFFFF;

    this->m_cached_block = (char*)cache_calloc(this->get_block_size());
    this->m_cached_block_number = 0xFFFFFFFF;

    this->m_cached_block2 = (char*)cache_calloc(this->get_block_size());
    this->m_cached_block2_number = 0xFFFFFFFF;

    this->m_cached_block3 = (char*)cache_calloc(this->get_block_size());
    this->m_cached_block3_number = 0xFFFFFFFF;

    this->m_block_size = (Ext2FS_Base_Blocksize << this->get_superblock().basic_superblock.block_size);
}
uint32_t Ext2Filesystem::get_file_size(const char* filename)
{
    uint32_t inode = this->find_file(filename);
    if(inode != 0xFFFFFFFF)
        return this->get_inode_filesize(inode);
    else
        return 0;
}

bool Ext2Filesystem::read(const char* filename,uint16_t* buf,uint32_t bytesCount)
{
    uint32_t inode_num = this->find_file(filename);
    if(inode_num == 0xFFFFFFFF)
        return false;
    uint32_t filesize;
    if(bytesCount == 0)
        filesize = this->get_inode_filesize(inode_num);
    else
        filesize = bytesCount;
    this->read_inode_data(inode_num,(char*)buf,filesize);
    return true;

}

uint32_t Ext2Filesystem::find_file(const char* filename)
{
    const char* tmp = filename;
    if(tmp[0] == '/')
        tmp = &tmp[1];

    uint32_t current_element_length;
    uint32_t directory_inode = Ext2FS_Root_Directory_Inode;
    uint32_t inode_num = 0xFFFFFFFF;
    
    while(1)
    {
        if(tmp[0] == (char)0x0)
            break;
        current_element_length = this->get_element_length(tmp);
        inode_num = this->get_inode_in_directory(directory_inode,(tmp),current_element_length);
        if(inode_num == 0xFFFFFFFF)
            return 0xFFFFFFFF;
        tmp = this->get_next_foldername(tmp);
        if(tmp[0] == '/')
            tmp = &tmp[1];
        directory_inode = inode_num;
    }
    return inode_num;
}
uint32_t Ext2Filesystem::get_element_length(const char* str)
{
    int max = strlen(str);
    int count = 0;
    int i=0;
    if(str[i] == '/')
        i++;
    while(i < max)
    {
        if(str[i] == '/')
            break;
        i++;
        count++;
    }
    return count;
}
const char* Ext2Filesystem::get_next_foldername(const char* str)
{
    int max = strlen(str);
    int i=0;
    if(str[i] == '/')
        i++;
    while(i < max)
    {
        if(str[i] == '/')
            break;
        i++;
    }
    return (str+i);
}

uint32_t Ext2Filesystem::get_inode_filesize(uint32_t inode)
{
    this->read_inode(inode);
    return (this->get_cached_inode().disk_sectors_used_count * this->partition->get_sector_size());
}

Ext2::Extended_Superblock& Ext2Filesystem::get_superblock()
{
    Ext2::Extended_Superblock* cached_superblock = (Ext2::Extended_Superblock*)m_cached_superblock;
    return *cached_superblock;
}
Ext2::Inode& Ext2Filesystem::get_cached_inode()
{
    Ext2::Inode* cached_inode = (Ext2::Inode*)m_cached_inode;
    return *cached_inode;
}
char* Ext2Filesystem::get_cached_block()
{
    return (char*)this->m_cached_block;
}
Ext2::BlockGroupDescriptor& Ext2Filesystem::get_cached_blockgroup()
{
    Ext2::BlockGroupDescriptor* cached_blockgroup = (Ext2::BlockGroupDescriptor*)m_cached_blockgroup;
    return *cached_blockgroup;
}

void Ext2Filesystem::read_inode_data(uint32_t inode,char* buf,uint32_t bytesCount)
{
    uint16_t block_size = this->get_block_size();
    uint32_t offset_blk_ptr = 0;
    this->read_inode(inode);

    uint32_t count;
    if((bytesCount % block_size) == 0)
        count = bytesCount/block_size;
    else
        count = (bytesCount/block_size) + 1;
    
    uint32_t buffer = (uint32_t)buf;
    uint32_t block=0;
    
    while(count > 0)
    {
        block = this->get_block_pointer(offset_blk_ptr);
        if(block != 0xFFFFFFFF)
        {
            this->read_block_to_buffer((char*)buffer,block);
            count = count - 1;
        }
        buffer += (uint32_t)this->get_block_size();
        offset_blk_ptr += 1;
    }
}
void Ext2Filesystem::read_inode(uint32_t inode)
{
    if(inode != 0xFFFFFFFF)
    {
        uint32_t blockgroup = ((inode - 1)/this->get_blockgroup_inodes_count());
        uint32_t index = ((inode - 1) % this->get_blockgroup_inodes_count());
        this->read_blockgroup_descriptor(blockgroup);
        uint32_t inode_table_block = get_cached_blockgroup().inode_table_blkaddr;
        uint32_t offset = (inode_table_block * this->get_block_size()) + ((index*this->get_inode_size()));    
        this->partition->read(0,0,offset,(uint16_t*)this->m_cached_inode,this->get_inode_size());
    }
    
}
void Ext2Filesystem::read_block_to_buffer(char* buf,uint32_t block1)
{
    uint32_t block_size = this->get_block_size();
    if(block1 != 0xFFFFFFFF)
    {
        this->partition->read(0,0,(block1*block_size),(uint16_t*)buf,block_size);
    }
}

void Ext2Filesystem::read_data_block(uint32_t block)
{
    if(this->m_cached_data_block_number != block)
    {
        this->partition->read(0,0,(block*this->get_block_size()),(uint16_t*)this->m_cached_data_block,this->get_block_size());
        this->m_cached_data_block_number = block;
    }
}
void Ext2Filesystem::read_block1(uint32_t block)
{
    if(this->m_cached_block_number != block)
    {
        this->partition->read(0,0,(block*this->get_block_size()),(uint16_t*)this->m_cached_block,this->get_block_size());
        this->m_cached_block_number = block;
    }
}
void Ext2Filesystem::read_block2(uint32_t block)
{
    if(this->m_cached_block2_number != block)
    {
        this->partition->read(0,0,(block*this->get_block_size()),(uint16_t*)this->m_cached_block2,this->get_block_size());
        this->m_cached_block2_number = block;
    }
}
void Ext2Filesystem::read_block3(uint32_t block)
{
    if(this->m_cached_block3_number != block)
    {
        this->partition->read(0,0,(block*this->get_block_size()),(uint16_t*)this->m_cached_block3,this->get_block_size());
        this->m_cached_block3_number = block;
    }
}
void Ext2Filesystem::transfer_block(char* buf,uint16_t limit)
{
    for(uint16_t i=0; i<limit;i++)
        buf[i] = this->m_cached_block[i];
}


uint32_t Ext2Filesystem::get_inode_in_directory(uint32_t directory_inode,const char* str,uint32_t element_length)
{
    this->read_inode(directory_inode);
    char* block;
    Ext2::DirectoryEntry* directory_entry;
    uint16_t length = element_length;
    for(uint32_t i=0; i<this->get_inode_count_blkpointers(directory_inode);++i)
    {
        this->read_data_block(get_cached_inode().direct_blk_ptr[i]);
        block = this->m_cached_data_block;
        directory_entry = (Ext2::DirectoryEntry*)block;
        int count = 0;
        while(count < this->get_block_size())
        {
            if(strncmp(str,directory_entry->name,length))
                return directory_entry->inode;
            count += directory_entry->entry_size;
            block += directory_entry->entry_size;
            directory_entry = (Ext2::DirectoryEntry*)(block);
        }
    }
    return 0xFFFFFFFF;
}


void Ext2Filesystem::read_blockgroup_descriptor(uint32_t blockgroup)
{
    uint16_t offset;
    if(this->get_block_size() > Ext2FS_Base_Blocksize)
        offset = this->get_block_size() + (sizeof(Ext2::BlockGroupDescriptor)*blockgroup);
    else
        offset = Ext2FS_Superblock_Offset+(sizeof(Ext2::Extended_Superblock)) + (sizeof(Ext2::BlockGroupDescriptor)*blockgroup);        
    this->partition->read(0,0,(offset),(uint16_t*)this->m_cached_blockgroup,sizeof(Ext2::BlockGroupDescriptor));
}


uint32_t Ext2Filesystem::get_blockgroup_inodes_count()
{
    return get_superblock().basic_superblock.block_group_inodes_count;
}
uint16_t Ext2Filesystem::get_inode_size()
{
    if(this->get_superblock().basic_superblock.major_version_number >= 1)
        return this->get_superblock().inode_bytes_size;
    else
        return Ext2FS_Inode_Fixed_Size;
}
uint16_t Ext2Filesystem::get_block_size() const
{
    return this->m_block_size;
}
uint32_t Ext2Filesystem::get_first_usable_inode()
{
    if(this->get_superblock().basic_superblock.major_version_number >= 1)
        return this->get_superblock().first_nonreserved_inode;
    else
        return Ext2FS_First_Nonreserved_Inode;
}


uint32_t Ext2Filesystem::get_block_pointer(uint32_t offset)
{
    if(offset < 12)
        return this->get_direct_block_pointer(offset);
    uint32_t offset2 = offset - 12;
    if(offset2 < (this->get_block_size()/sizeof(uint32_t)))
        return this->get_indirect_block_pointer(this->get_cached_inode().singly_indirect_blk_ptr,offset2);
    if(offset2 < ((this->get_block_size()/sizeof(uint32_t))*(this->get_block_size()/sizeof(uint32_t))))
        return this->get_double_indirect_block_pointer(this->get_cached_inode().doubly_indirect_blk_ptr,offset2);
    if(offset2 < ((this->get_block_size()/sizeof(uint32_t))*(this->get_block_size()/sizeof(uint32_t))*(this->get_block_size()/sizeof(uint32_t))))
        return this->get_triple_indirect_block_pointer(this->get_cached_inode().triply_indirect_blk_ptr,offset2);   
    return 0xFFFFFFFF;
}
uint32_t Ext2Filesystem::get_direct_block_pointer(uint8_t offset)
{
    if(offset < 12)
        if(this->get_cached_inode().direct_blk_ptr[offset] == 0)
            return 0xFFFFFFFF;
        else
            return (uint32_t)this->get_cached_inode().direct_blk_ptr[offset];
    else
        return 0xFFFFFFFF;
}
uint32_t Ext2Filesystem::get_indirect_block_pointer(uint32_t indirect_blk_ptr,uint32_t offset)
{
    if(indirect_blk_ptr == 0)
        return 0xFFFFFFFF;
    this->read_block1(indirect_blk_ptr);
    uint32_t* tmp = (uint32_t*)this->m_cached_block;
    if(offset < (this->get_block_size()/sizeof(uint32_t)))
        return tmp[offset];
    else
        return 0xFFFFFFFF;
}
uint32_t Ext2Filesystem::get_double_indirect_block_pointer(uint32_t indirect_blk_ptr,uint32_t offset)
{
    if(indirect_blk_ptr == 0)
        return 0xFFFFFFFF;
    this->read_block2(indirect_blk_ptr);
    uint32_t block = offset / (this->get_block_size() / sizeof(uint32_t));
    uint32_t offset2 = offset % (this->get_block_size() / sizeof(uint32_t));
    return this->get_indirect_block_pointer(block,offset2);
}
uint32_t Ext2Filesystem::get_triple_indirect_block_pointer(uint32_t indirect_blk_ptr,uint32_t offset)
{
    if(indirect_blk_ptr == 0)
        return 0xFFFFFFFF;
    this->read_block3(indirect_blk_ptr);
    uint32_t block = offset / ((this->get_block_size() / sizeof(uint32_t))*(this->get_block_size() / sizeof(uint32_t)));
    uint32_t offset2 = offset % ((this->get_block_size() / sizeof(uint32_t))*(this->get_block_size() / sizeof(uint32_t)));
    return this->get_double_indirect_block_pointer(block,offset2);
}
uint32_t Ext2Filesystem::get_inode_count_blkpointers(uint32_t inode)
{
    this->read_inode(inode);
    if(((this->get_cached_inode().disk_sectors_used_count * this->partition->get_sector_size()) % this->get_block_size()) == 0)
        return ((this->get_cached_inode().disk_sectors_used_count * this->partition->get_sector_size())  / this->get_block_size());
    else
        return ((this->get_cached_inode().disk_sectors_used_count * this->partition->get_sector_size())  / this->get_block_size()) + 1;
}
uint32_t Ext2Filesystem::get_blkpointers_count_by_bytes(uint32_t bytesCount)
{
    uint32_t disk_sectors_used_count;
    if((bytesCount % this->partition->get_sector_size()) == 0)
        disk_sectors_used_count = bytesCount / this->partition->get_sector_size();
    else
        disk_sectors_used_count = (bytesCount / this->partition->get_sector_size()) + 1;
    
    if(((disk_sectors_used_count * this->partition->get_sector_size()) % this->get_block_size()) == 0)
        return ((disk_sectors_used_count * this->partition->get_sector_size())  / this->get_block_size());
    else
        return ((disk_sectors_used_count * this->partition->get_sector_size())  / this->get_block_size()) + 1;
}
