/*

Tool for mounting OpenVZ ploop images (openvz.org/Ploop) without support from kernel side on any kernel. Only read only mounting supported now. 
License: GPLv2
Author: pavel.odintsov@gmail.com

*/

#include <fstream>
#include <iostream>
#include <sys/types.h>

#ifdef  __linux__
#include <linux/types.h>
#endif

#ifdef __APPLE__

#endif
 
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>

#include <vector>
#include <map>

#include "buse.h"

using namespace std;


// Смещение, которое нужно сделать, чтобы добраться до карты. Первые 64 байта - это хидер
// sizeof(struct ploop_pvd_header) / sizeof(u32)
#define PLOOP_MAP_OFFSET 16

/* Compressed disk v1 signature */
#define SIGNATURE_STRUCTURED_DISK_V1 "WithoutFreeSpace"

/* Compressed disk v2 signature */
#define SIGNATURE_STRUCTURED_DISK_V2 "WithouFreSpacExt"

/* Bytes in sector */
#define BYTES_IN_SECTOR 512

/* This defines slot in mapping page. Right now it is 32 bit
 * and therefore it directly matches ploop1 structure. */
typedef __u32 map_index_t;

/* Эти переменные вынуждены быть глобальными, так как иного варианта работы с ними в BUSE нету */
typedef std::map<u_int64_t, map_index_t> bat_table_type;
bat_table_type ploop_bat;
int ploop_global_file_handle = 0;
/* Cluster size in bytes */
int global_ploop_cluster_size = 0;
/* Ploop version */ 
int global_ploop_version = 2;
__u64 global_first_block_offset = 0;
__u64 global_ploop_file_size_on_underlying_fs = 0;
__u64 global_ploop_block_device_size = 0;
static int buse_debug = 1;

bool TRACE_REQUESTS = 0;

// BAT block format:
// https://github.com/pavel-odintsov/openvz_rhel6_kernel_mirror/blob/master/drivers/block/ploop/map.c
// Очень важная функция в ядре: drivers/block/ploop/fmt_ploop1.c, ploop1_open

// Структура взята: http://git.openvz.org/?p=ploop;a=blob;f=include/ploop1_image.h
#pragma pack(push,1)
struct ploop_pvd_header
{
    __u8  m_Sig[16];          /* Signature */
    __u32 m_Type;             /* Disk type */
    __u32 m_Heads;            /* heads count */
    __u32 m_Cylinders;        /* tracks count */
    __u32 m_Sectors;          /* Sectors per track count */
    __u32 m_Size;             /* Size of disk in tracks */
    union {                   /* Size of disk in 512-byte sectors */
        struct {
            __u32 m_SizeInSectors_v1;
            __u32 Unused;
        };
        __u64 m_SizeInSectors_v2;
    };
    __u32 m_DiskInUse;        /* Disk in use */
    __u32 m_FirstBlockOffset; /* First data block offset (in sectors) */
    __u32 m_Flags;            /* Misc flags */
    __u8  m_Reserved[8];      /* Reserved */
};
#pragma pack(pop)

/* Prototypes */
int ploop_read_as_block_device(void *buf, u_int32_t len, u_int64_t offset);
bool find_ext4_magic(ploop_pvd_header* ploop_header, char* file_path, __u64 offset);
void consistency_check();
void read_bat(ploop_pvd_header* ploop_header, char* file_path, bat_table_type& ploop_bat);
bool is_digit_line(char* string);
int file_exists(char* file_path);
int get_ploop_version(struct ploop_pvd_header* header);
__u64 get_ploop_size_in_sectors(struct ploop_pvd_header* header);

/* Functions */
__u64 get_ploop_size_in_sectors(struct ploop_pvd_header* header) {
    int ploop_version = get_ploop_version(header);
    __u64 disk_size = 0;

    if (ploop_version == 1) {
        disk_size = header->m_SizeInSectors_v1;
    } else if (ploop_version == 2) {
        disk_size = header->m_SizeInSectors_v2;
    } else {
        printf("Unexpected ploop version");
        exit(1);
    }

    return disk_size; 
}

void print_ploop_header(struct ploop_pvd_header* header) {
    int ploop_version = get_ploop_version(header);

    printf("version: %d disk type: %d heads count: %d cylinder count: %d sector count: %d size in tracks: %d ", ploop_version, header->m_Type , header->m_Heads, header->m_Cylinders,
        header->m_Sectors, header->m_Size);

    __u64 disk_size = get_ploop_size_in_sectors(header);
    printf("size in sectors: %llu ", disk_size);

    printf("disk in use: %d first block offset: %d flags: %d",
        header->m_DiskInUse, header->m_FirstBlockOffset, header->m_Flags);

    printf("\n");
}

int get_ploop_version(struct ploop_pvd_header* header) {
    if (!memcmp(header->m_Sig, SIGNATURE_STRUCTURED_DISK_V1, sizeof(header->m_Sig))) {
        return 1;
    }

    if  (!memcmp(header->m_Sig, SIGNATURE_STRUCTURED_DISK_V2, sizeof(header->m_Sig))) {
        return 2;
    }

    return -1;
}


int file_exists(char* file_path) {
    struct stat stat_data;

    int stat_res = stat(file_path, &stat_data);

    if (stat_res == 0) {
        return 1;
    } else {
        return 0;
    }
}

#define GPT_SIGNATURE 0x5452415020494645ULL 
void read_gpt(ploop_pvd_header* ploop_header, char* file_path, int* result) {
    unsigned long long guid_header;

    // GPT table starts from 512byte on 512b sector or from 4096byte with 4k sector
    ploop_read_as_block_device((void *)&guid_header, sizeof(guid_header), BYTES_IN_SECTOR);

    if (guid_header == GPT_SIGNATURE) {
        *result = 1;
    } else {
        *result = 0;
    }
}

// Функция для поиска сигнатур файловой системы ext4
bool find_ext4_magic(ploop_pvd_header* ploop_header, char* file_path, __u64 offset) {
    __u16 ext4_magic = 0;
    
    // 0x438 - смещение magic short uint внутри файловой системы
    ploop_read_as_block_device((void *)&ext4_magic, sizeof(ext4_magic), offset + 0x438);
    
    // magic number взят /usr/share/misc/magic 
    if (ext4_magic == 0xEF53) {
        return true;
    } else {
        return false;
    }    
}

void read_header(ploop_pvd_header* ploop_header, char* file_path) {
    cout<<"We process: "<<file_path<<endl;

    struct stat stat_data;

    if (stat(file_path, &stat_data) != 0) {
        std::cout<<"Can't get stat data";
        exit(1);
    }

    global_ploop_file_size_on_underlying_fs = stat_data.st_size;
    std::cout<<"Ploop file size is: "<<stat_data.st_size<<endl;

    int file_handle =  open(file_path, O_RDONLY);

    if (file_handle) {
        int pread_result = pread(file_handle, (char*)ploop_header, sizeof(ploop_pvd_header), 0);

        if (pread_result == -1) {
            cout<<"Can't read header!"<<endl;
            exit(1);
        }

        close(file_handle);

        print_ploop_header(ploop_header);
    } else {
        std::cout<<"Can't open ploop file"<<endl;
        exit(1);
    }
}

// Прочесть BAT таблицу
void read_bat(ploop_pvd_header* ploop_header, char* file_path, bat_table_type& ploop_bat) {
    int file_handle =  open(file_path, O_RDONLY);

    if (!file_handle) {
        std::cout<<"Can't open ploop file"<<endl;
        exit(1);
    }

    // Размер блока ploop в байтах
    int cluster_size = ploop_header->m_Sectors * BYTES_IN_SECTOR;

    global_ploop_version = get_ploop_version(ploop_header); 
    global_ploop_cluster_size = cluster_size;

    // Смещение первого блока с данными в байтах
    int first_data_block_offset = ploop_header->m_FirstBlockOffset * BYTES_IN_SECTOR;
      
    global_first_block_offset = first_data_block_offset;
 
    // возьмем объем диска в секторах
    __u64 disk_size = get_ploop_size_in_sectors(ploop_header) * BYTES_IN_SECTOR;

    global_ploop_block_device_size = disk_size;

    if (disk_size % cluster_size != 0) {
        cout<<"Disk size can't be counted in ploop clusters"<<endl;
        exit(1);
    }

    __u64 disk_size_in_ploop_blocks = disk_size / cluster_size;
    cout<<"For storing "<<disk_size<< " bytes on disk we need "<<disk_size_in_ploop_blocks<< " ploop blocks"<<endl;

    // Теперь зная размер блока и смещение блока с данными можно посчитать возможное число блоков BAT
    if (first_data_block_offset % cluster_size != 0) {
        cout <<"Internal error! Data offset should be in start of cluster"<<endl;
        exit(1);
    }

    // Один BAT может адресовать около 250 гб данных
    int bat_blocks_number = first_data_block_offset/cluster_size; 

    cout<<"We have "<<bat_blocks_number<<" BAT blocks"<<endl;

    // Общее число не пустых блоков во всех BAT
    int not_null_blocks = 0;
 
    // глобальный индекс в таблице маппинга 
    int global_index = 0;

    // всегда выделяем объем данных по размеру кластера
    map_index_t* ploop_map = (map_index_t*)malloc(cluster_size);

    lseek(file_handle, sizeof(ploop_pvd_header), SEEK_SET);

    for (int bat_index = 0; bat_index < bat_blocks_number; bat_index ++) {
        int map_size = 0;

        if (bat_index == 0) { 
            // первый BAT блок у нас после заголовка ploop
            map_size = cluster_size - sizeof(ploop_pvd_header);
        } else {
            // а тут весь блок наш, от начала до самого конца
            map_size = cluster_size;
        }

        // read data from disk
        int read_result = read(file_handle, (char*)ploop_map, map_size); 
  
        if (read_result == -1) {
            cout<<"Can't read map from file!"<<endl;
            exit(1);
        }   

 
        // вообще если размер блока нестандартный и на гранцие блока попадется половина байта и весь не влезет
        // то будет косяк
        if (map_size % sizeof(map_index_t) != 0) {
            cout<<"Internal error! Can't count number of slots in map because it's not an integer"<<endl;
            exit(1);
        }

        int number_of_slots_in_map = map_size / sizeof(map_index_t);

        cout<<"We have "<<number_of_slots_in_map<<" slots in "<<bat_index + 1<<" map"<<endl;

        for (int i = 0; i < number_of_slots_in_map; i++) {
            if (ploop_map[i] != 0) {
                if (global_index > disk_size_in_ploop_blocks) {
                    cout<<"Unexpected not null block in free zone! Index: "<<global_index<<" data: "<<ploop_map[i]<<endl;
                }
        
                // заносим в общую таблицу размещения файлов не пустые блоки
                // Как можно догадаться, global_index начинается с нуля, а вот целевой блок, всегда считается с 1,
                // так как 0 означает, что блок пуст
                ploop_bat[global_index] = ploop_map[i];
                not_null_blocks++;
            }

            global_index++;
        }
    }

    free(ploop_map);
    close(file_handle);
    
    std::cout<<"Number of non zero blocks in map: "<<not_null_blocks<<endl;

    vector<map_index_t> used_blocks;
    used_blocks.reserve(not_null_blocks);
    for (bat_table_type::iterator ii = ploop_bat.begin(); ii != ploop_bat.end(); ++ii) {
        //std::cout<<"index: "<<ii->first<<" key: "<<ii->second<<endl;
        used_blocks.push_back(ii->second);
    } 
}

int ploop_read(void *buf, u_int32_t len, u_int64_t offset, void *userdata) {
    return ploop_read_as_block_device(buf, len, offset); 
};

// Функция обертка, чтобы читать ploop как блочное устройство 
int ploop_read_as_block_device(void *buf, u_int32_t len, u_int64_t offset) {
    int file_handle = ploop_global_file_handle;

    __u64 data_read_end = offset + len;

    if (TRACE_REQUESTS) {
        cout<<"We got request for reading from offset: "<<offset<<" length "<<len<< " bytes to the end: "<<data_read_end<<endl;
    }

    if (global_ploop_block_device_size < offset) {
        cout<<"!!!ERROR!!! Somebody wants read with offset bigger than our ploop device"<<endl; 
    }

    if (global_ploop_block_device_size < data_read_end) {
         cout<<"!!!ERROR!!! END OF DATA IS OUT OF PLOOP DEVICE SIZE!!! IN'S AN MISTAKE"<<endl;
    }

    assert(global_first_block_offset != 0);
    assert(global_ploop_cluster_size != 0);

    u_int64_t data_page_number = offset / global_ploop_cluster_size;
    u_int64_t data_page_offset = offset % global_ploop_cluster_size;

    bat_table_type::iterator map_item = ploop_bat.find(data_page_number);

    if (map_item == ploop_bat.end()) {
        if (TRACE_REQUESTS) {
            cout<<"WARNING! WARNING! WARNING! We can't get mapping for ploop block "<<data_page_number<<endl;
        }

        // It's normal because ploops is sparse and sometimes ext4 tries to read blank areas
        memset(buf, 0, len);

        return 0;
    }

    // Где данные находятся реально
    map_index_t data_page_real_place = map_item->second;

    assert(data_page_real_place != 0);

    
    u_int64_t position_in_file = 0;

    if (global_ploop_version == 2) {
        // Для второй версии ploop data_page_real_place считается в блоках
        position_in_file = global_first_block_offset + (data_page_real_place-1) * global_ploop_cluster_size + data_page_offset;
    } else {
        printf("Target position: %d\n", data_page_real_place);
        // А для первой, в 512 байтовых секторах: data_page_real_place
        // Причем, реальное местоположение хранится в BAT с учетом смещения хидера
        position_in_file = data_page_real_place * 512 + data_page_offset;
    }

    // Если смещение в файле + длина считываемых данных больше самого файла
    if (position_in_file + len > global_ploop_file_size_on_underlying_fs) {
        cout<<"WE TRIED TO READ DATA OVER THE FILE END!!! IT'S REALLY BAD"<<endl;
    } 

    // Тут мы рассматриваем случай, когда данные попадают на два блока одновременно 
    // TODO: реализовать
    if (data_page_offset + len > global_ploop_cluster_size) {
        cout<<"DATA IS OVERLAP WITH NEXT BLOCK!!! NOT IMPLEMENTED YET!!!"<<endl;
    }

    if (TRACE_REQUESTS) {
        cout<<"ploop cluster size:\t\t"         <<global_ploop_cluster_size<<endl;
        cout<<"global first block offset:\t"  <<global_first_block_offset<<endl;
        cout<<"global ploop block device size\t"<<global_ploop_block_device_size<<endl;
        cout<<"data_page_number:\t\t"          <<data_page_number<<endl;
        cout<<"data_page_real_place:\t\t"       <<data_page_real_place<<endl;
        cout<<"offset for current page:\t"    <<data_page_offset<<endl;
        cout<<"ploop file size:\t\t"           <<global_ploop_file_size_on_underlying_fs<<endl;
        cout<<"position_in_file:\t\t"          <<position_in_file<<endl;
        cout<<endl;
    }

    size_t pread_result = pread(ploop_global_file_handle, (void*)buf, len, position_in_file);
   
    if (pread_result == -1) {
        cout<<"Can't read data from ploop file for nbd!"<<endl;
        exit(1);
    }   

    if (pread_result < len) {
        cout<<"We readed ("<<pread_result<<") less data then client requested ("<<len<<")"<<endl;
    }
 
    return 0;
}

static int ploop_write(const void *buf, u_int32_t len, u_int64_t offset, void *userdata) {
    if (*(int *)userdata) {
        fprintf(stderr, "W - %lu, %u\n", offset, len);
    }

    return 0;
}

static void ploop_disc(void *userdata) {
  (void)(userdata);
  fprintf(stderr, "Received a disconnect request.\n");
}

static int ploop_flush(void *userdata) {
  (void)(userdata);
  fprintf(stderr, "Received a flush request.\n");
  return 0;
}

static int ploop_trim(u_int64_t from, u_int32_t len, void *userdata) {
  (void)(userdata);
  fprintf(stderr, "T - %lu, %u\n", from, len);
  return 0;
}


static struct buse_operations ploop_userspace;

void init_ploop_userspace(__u64 disk_size_in_bytes) {
    ploop_userspace.read = ploop_read;
    ploop_userspace.size = disk_size_in_bytes;
    ploop_userspace.blksize = BYTES_IN_SECTOR;

    //ploop_userspace.write = ploop_write;
    //ploop_userspace.disc = ploop_disc;
    //ploop_userspace.flush = ploop_flush;
    //ploop_userspace.trim = ploop_trim;
}

bool is_digit_line(char* string) {
    for (int i = 0; i < strlen(string); i++) {
        if(!isdigit(string[i])) {
            return false;
        }
    }

    return true;
}

void consistency_check() {
    if (sizeof(ploop_pvd_header) != 64) {
        std::cout<<"UNEXPECTED BAHAVIOUR!!!! INCORRECT SIZE OF PLOOP HEADER"<<endl;
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    if (getenv("TRACE_REQUESTS")) {
        TRACE_REQUESTS = true;
    }


    consistency_check();

    if (argc < 2) {
        cout<<"Please specify CTID or path to ploop image"<<endl;
        exit(1);
    } 

    char* nbd_device_name = (char*)"/dev/nbd0";
    char file_path[256];

    if (is_digit_line(argv[1])) {
        // We got CTID number
 
        vector<string> root_hdd_paths;
        // openvz
        root_hdd_paths.push_back("/vz/private/%s/root.hdd/root.hdd");
        // parallels cloud server
        root_hdd_paths.push_back("/vz/private/%s/root.hdd/root.hds");
        // openvz at Debian
        root_hdd_paths.push_back("/var/lib/vz/private/%s/root.hdd/root.hdd");

        int we_found_root_hdd = false;
        for( vector<string>::iterator ii = root_hdd_paths.begin(); ii != root_hdd_paths.end(); ++ii) {
            sprintf(file_path, ii->c_str(), argv[1]);

            // If we found file we stop loop
            if (file_exists(file_path)) {
                we_found_root_hdd = true;
                break;
            }
        }

        if (!we_found_root_hdd) {
            cout<<"We cant find root.hdd for this container"<<endl;
            exit(1);
        } 
    } else {
        // We got path to root.hdd
        sprintf(file_path, "%s", argv[1]);

        if (!file_exists(file_path)) {
            cout<<"Path "<<file_path<<" is not exists, please check CTID number"<<endl;
            exit(1);
        }   
    }

    ploop_pvd_header* ploop_header = new ploop_pvd_header;

    // read header
    read_header(ploop_header, file_path);

    // read BAT tables
    read_bat(ploop_header, file_path, ploop_bat);

    // open ploop file for read_gpt and find_ext4
    ploop_global_file_handle = open(file_path, O_RDONLY);

    if (ploop_header->m_DiskInUse) {
        cout<<"Please be careful because this disk used now! If you need consistent backup please stop VE"<<endl;
    }

    // read GPT header
    int gpt_is_found = 0;
    read_gpt(ploop_header, file_path, &gpt_is_found);

    if (gpt_is_found) {
        cout<<"We found GPT table on this disk"<<endl;
    } else {
        cout<<"!!!ERROR!!! We can't found GPT table on this disk"<<endl;
    }

    bool ext4_found = false;
    bool we_should_mount_whole_disk = false;

    // По-хорошему, конечно же, нужно читать GPT, но мы знаем, что ploop создает первый раздел
    // по смещению первого блока данных
    if (find_ext4_magic(ploop_header, file_path, global_ploop_cluster_size)) {
        ext4_found = true;
        cout<<"We found ext4 signature at first partition block"<<endl;
    }

    // Также из-за багов в скрипте vps_reinstall фс может быть создана прямо на первом блоке диска
    if (find_ext4_magic(ploop_header, file_path, 0)) {
        ext4_found = true;
        we_should_mount_whole_disk = true;
        cout<<"We found ext4 signature at first block of disk. This fs created over block device without GPT"<<endl;
    }

    if (!ext4_found) {
        cout<<"!!!ERROR!!! We can't find ext4 signature"<<endl;
    }


    if (getenv("SKIP_MOUNT")) {
        exit(0);
    }   


    __u64 ploop_size = get_ploop_size_in_sectors(ploop_header);
    init_ploop_userspace(ploop_size * BYTES_IN_SECTOR);

    system("modprobe nbd max_part=16 2>&1 >/dev/null");
    int is_read_only = 1;

    // set device as read only
    if (is_read_only) {
        char blockdev_command[256];
        sprintf(blockdev_command, "blockdev --setro %s 2>&1 >/dev/null", nbd_device_name);

        cout<<"Set device "<<nbd_device_name<<" as read only\n";
        system(blockdev_command);
    }

    // we need close and reopen handles for fork
    close(ploop_global_file_handle);

    if (fork()) {
        //parent
        sleep(2);
   
        if (!we_should_mount_whole_disk) { 
            cout<<"Try to found partitions on ploop device"<<endl;
            /*
            partx lacks convinient support of NBD devices:
            HDIO_GETGEO: Inappropriate ioctl for device 
            char partx_command[128];
            sprintf(partx_command, "partx -a %s 2>&1 >/dev/null", nbd_device_name);
            system(partx_command);
            */

            char partprobe_command[128];
            sprintf(partprobe_command, "partprobe %s", nbd_device_name);
            system(partprobe_command);
        }

        char first_nbd_partition_path[256];

        if (we_should_mount_whole_disk) {
            sprintf(first_nbd_partition_path, "%s", nbd_device_name);
        } else {
            sprintf(first_nbd_partition_path, "%sp1", nbd_device_name);
        }

        if (!file_exists(first_nbd_partition_path)) {
            cout<<"First ploop partition was not detected properly, please call partx/partprobe manually"<<endl;
        }

        cout<<"You could mount ploop filesystem with command: "<<"mount -r -o noload "<<first_nbd_partition_path<<" /mnt"<<endl;
       
        // wait for processes
        int status = 0;
        wait(&status);
    } else {
        ploop_global_file_handle = open(file_path, O_RDONLY);
        buse_main(nbd_device_name, &ploop_userspace, (void *)&buse_debug);
    }
    // close(ploop_global_file_handle)
    // delete (ploop_header);
}
