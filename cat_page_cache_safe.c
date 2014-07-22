// We need do this for enable O_DIRECT
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

// cat like tool which bypass Linux Page cache

/* 

./a.out /vz/template/cache/centos-6-x86_64.tar.gz |pv >/dev/null

*/

int main(int argc, char *argv[]) {
    // We use 512 KB buffer for reading data
    size_t buffer_size = 16 * 1024;
    // We need use buffer aligned for block device size for O_DIRECT
    size_t alignment_block = 512;

    char file_path[256];
    void* buffer = NULL;

    if (argc < 2) {
        fprintf(stderr, "Please provide path to file as argument\n");
        exit(1);
    }

    strcpy(file_path, argv[1]);

    printf("We will process %s\n", file_path); 

    int file = open(file_path, O_RDONLY|O_DIRECT);

    if (!file) {
        fprintf(stderr, "Can't open file %s for reading with O_DIRECT flag %s\n", file_path, strerror(errno)); 
        exit(1);
    } 

    int ret = posix_memalign(&buffer, alignment_block, buffer_size);

    if (ret != 0) {
        fprintf(stderr, "Can't allocate aligned memory for O_DIRECT reading\n");
        exit(1);
    }

    size_t readed_bytes = 0;

    while (1) {
        // TODO: add support for sendfile
        size_t readed_bytes = read(file, buffer, buffer_size);
       
        if (readed_bytes < 0) { 
            // readed_bytes == -1
            fprintf(stderr, "Can't read data from source file with error %s\n", strerror(errno));
            break;
        } else if (readed_bytes == 0) {
            // EOF
            break;
        } else {
            // readed_bytes > 0
            size_t writed_bytes = write(stdout, buffer, readed_bytes);

            if (writed_bytes == -1) {
                fprintf(stderr, "Can't write to stdout with error %s\n", strerror(errno));
            } 
        } 
    } 

    free(buffer);
    close(file);
} 
