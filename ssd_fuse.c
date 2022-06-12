/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME "ssd_file"
// lba  = page  = low 16 bits
// nand = block = high 16 bits
#define PCA_ADDR(pca) ((pca & 0xffff) + ((pca >> 16) * PAGE_PER_BLOCK))

enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

/*
    0 = free
    1 = valid
    2 = invalid
*/
int pages_status[PHYSICAL_NAND_NUM][PAGE_PER_BLOCK];

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int lba : 16;
        unsigned int nand: 16;
    } fields;
};

PCA_RULE curr_pca;
static unsigned int get_next_pca();

unsigned int* L2P,* P2L,* valid_count, free_block_number, gc_blockid;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size > NAND_SIZE_KB * 1024)
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }
}

static int ssd_expand(size_t new_size)
{
    //logic must less logic limit
    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    //read
    if ((fptr = fopen(nand_name, "r")))
    {
        fseek(fptr, my_pca.fields.lba * 512, SEEK_SET);
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}

static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    //write
    if ((fptr = fopen(nand_name, "r+")))
    {
        fseek(fptr, my_pca.fields.lba * 512, SEEK_SET);
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size++;
        valid_count[my_pca.fields.nand]++;
        // set this page status -> 1
        pages_status[my_pca.fields.nand][my_pca.fields.lba] = 1;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block_index)
{
    char nand_name[100];
    FILE* fptr;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block_index);
    fptr = fopen(nand_name, "w");
    if (fptr == NULL)
    {
        printf("erase nand_%d fail", block_index);
        return 0;
    }
    fclose(fptr);
    valid_count[block_index] = FREE_BLOCK;
    // set the status of the pages of this block -> 0
    for (int i = 0; i < PAGE_PER_BLOCK; i++)
    {
        pages_status[block_index][i] = 0;
    }
    return 1;
}

static unsigned int get_next_block()
{
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (valid_count[(curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM] == FREE_BLOCK)
        {
            curr_pca.fields.nand = (curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM;
            curr_pca.fields.lba = 0;
            free_block_number--;
            valid_count[curr_pca.fields.nand] = 0;
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}

void garbage_collection()
{
    int blockid, min_valid, lba, ret;
    char *buf;
    PCA_RULE my_pca;

    blockid = -1;
    min_valid = PAGE_PER_BLOCK + 1;

    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (valid_count[i] < min_valid)
        {
            min_valid = valid_count[i];
            blockid = i;
        }
    }

    // no space
    if (min_valid == PAGE_PER_BLOCK)
    {
        printf("[ERROR] NO MORE SPACE in GC\n");
    }

    buf = calloc(512, sizeof(char));
    // my_pca = block that will be erase
    my_pca.fields.nand = blockid;
    // curr_pca = GC_block (prev last block)
    curr_pca.fields.nand = gc_blockid;
    curr_pca.fields.lba = 0;

    for (int i = 0; i < PAGE_PER_BLOCK; i++)
    {
        // page is valid
        if (pages_status[blockid][i] == 1)
        {
            my_pca.fields.lba = i;
            ret = nand_read(buf, my_pca.pca);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO READ in GC\n");
                return 0;
            }
            ret = nand_write(buf, curr_pca.pca);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO WRITE in GC\n");
                return 0;
            }

            lba = P2L[PCA_ADDR(my_pca.pca)];
            L2P[lba] = curr_pca.pca;
            P2L[PCA_ADDR(curr_pca.pca)] = lba;
            P2L[PCA_ADDR(my_pca.pca)] = INVALID_LBA;
            
            curr_pca.fields.lba += 1;
        }
    }
    free(buf);
    ret = nand_erase(blockid);
    if (ret <= 0)
    {
        printf("[ERROR] FAIL TO ERASE in GC\n");
        return 0;
    }
    gc_blockid = blockid;
    return 0;
}

static unsigned int get_next_pca()
{
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        valid_count[0] = 0;
        free_block_number--;
        return curr_pca.pca;
    }

    if(curr_pca.fields.lba == 9)
    {
        // when curr_block is full & number of free block == 1
        // do garbage collection
        if (free_block_number == 1)
        {
            garbage_collection();
            return curr_pca.pca;
        }

        int temp = get_next_block();
        if (temp == OUT_OF_BLOCK)
        {
            return OUT_OF_BLOCK;
        }
        else if(temp == -EINVAL)
        {
            return -EINVAL;
        }
        else
        {
            return temp;
        }
    }
    else
    {
        curr_pca.fields.lba += 1;
    }
    return curr_pca.pca;
}

static int ftl_read(char* buf, size_t lba)
{
    int size;
    PCA_RULE my_pca;

    // find PCA from L2P
    my_pca.pca = L2P[lba];

    if (my_pca.pca == INVALID_PCA)
    {
        printf("[ERROR] INVALID_PCA in ftl_read\n");
        return 0;
    }

    size = nand_read(buf, my_pca.pca);
    return size;
}

static int ftl_write(const char* buf, size_t lba_range, size_t lba)
{
    int size, pca;

    pca = get_next_pca();

    if (pca == OUT_OF_BLOCK || pca == -EINVAL)
    {
        printf("[ERROR] INVALID_PCA in ftl_write\n");
        return 0;
    }

    size = nand_write(buf, pca);
    L2P[lba] = pca;
    P2L[PCA_ADDR(pca)] = lba;

    return size;
}

static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}

static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}

static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, ret;
    char* tmp_buf;

    //off limit
    if ((offset) >= logic_size)
    {
        return 0;
    }
    if (size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        ret = ftl_read(&tmp_buf[i * 512], tmp_lba + i);
        if (ret <= 0)
        {
            return 0;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}

static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, ret;
    char* tmp_buf;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    // use tmp_lba to find block & page
    int tmp_block, tmp_page;
    tmp_block = tmp_lba / PAGE_PER_BLOCK;
    tmp_page = tmp_lba % PAGE_PER_BLOCK;

    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));
    // if offset in free page
    if (pages_status[tmp_block][tmp_page] == 0)
    {
        printf("=========================In free page\n");
        memcpy(tmp_buf, buf, size);
        for (int i = 0; i < tmp_lba_range; i++)
        {
            ret = ftl_write(&tmp_buf[i * 512], tmp_lba_range - i, tmp_lba + i);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO WRITE IN ssd_do_write (offset in free page)\n");
                free(tmp_buf);
                return 0;
            }
        }
    }
    // if offset in valid page => Read-Modify-Write
    else if (pages_status[tmp_block][tmp_page] == 1)
    {
        printf("=========================In v page\n");
        int invalid_cnt = tmp_lba_range;
        // read
        ret = ftl_read(tmp_buf, tmp_lba);
        if (ret <= 0)
        {
            printf("[ERROR] FAIL TO READ IN ssd_do_write (offset in valid page)\n");
            free(tmp_buf);
            return 0;
        }
        // modify
        int prev_len = offset % 512;
        memset(&tmp_buf[prev_len], 0, 512);
        memcpy(&tmp_buf[prev_len], buf, size);
        // write
        // find next free page
        while (pages_status[tmp_block][tmp_page] >= 1)
        {
            // set invalid
            if (invalid_cnt > 0)
            {
                pages_status[tmp_block][tmp_page] = 2;
            }
            invalid_cnt -= 1;
            // next page
            tmp_lba += 1;
            tmp_block = tmp_lba / PAGE_PER_BLOCK;
            tmp_page = tmp_lba % PAGE_PER_BLOCK;
        }
        // tmp_lba = next free page => we can write
        for (int i = 0; i < tmp_lba_range; i++)
        {
            ret = ftl_write(&tmp_buf[i * 512], tmp_lba_range - i, tmp_lba + i);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO WRITE IN ssd_do_write (offset in valid page)\n");
                free(tmp_buf);
                return 0;
            }
        }
    }
    // if offset in invalid page
    else if (pages_status[tmp_block][tmp_page] == 2)
    {
        printf("=========================In Iv page\n");
        memcpy(tmp_buf, buf, size);
        // find next free page
        while (pages_status[tmp_block][tmp_page] >= 1)
        {
            tmp_lba += 1;
            tmp_block = tmp_lba / PAGE_PER_BLOCK;
            tmp_page = tmp_lba % PAGE_PER_BLOCK;
        }
        // tmp_lba = next free page => we can write
        for (int i = 0; i < tmp_lba_range; i++)
        {
            ret = ftl_write(&tmp_buf[i * 512], tmp_lba_range - i, tmp_lba + i);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO WRITE IN ssd_do_write (offset in invalid page)\n");
                free(tmp_buf);
                return 0;
            }
        }
    }
    free(tmp_buf);
    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{

    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    // reset all pages status & gc_blockid
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        for (int j = 0; j < PAGE_PER_BLOCK; j++)
        {
            pages_status[i][j] = 0;
        }
    }
    gc_blockid = PHYSICAL_NAND_NUM - 1;

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}

static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}

static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};

int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    gc_blockid = PHYSICAL_NAND_NUM - 1;
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        for (int j = 0; j < PAGE_PER_BLOCK; j++)
        {
            pages_status[i][j] = 0;
        }
    }

    L2P = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    valid_count = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
