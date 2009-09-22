/**
  hfsplusclone.c - part of Partclone project
 *
 * Copyright (c) 2007~ Thomas Tsai <thomas at nchc org tw>
 *
 * read hfsplus super block and bitmap
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <malloc.h>
#include <stdarg.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <linux/types.h>

#include "partclone.h"
#include "hfsplusclone.h"
#include "progress.h"
#include "fs_common.h"

struct HFSPlusVolumeHeader sb;
int ret;
char *EXECNAME = "partclone.hfsp";
extern fs_cmd_opt fs_opt;

static short reverseShort(short s){
    unsigned char c1, c2;

    c1 = s & 255;
    c2 = (s >> 8) & 255;

    return (c1 << 8)+c2;
}

static int reverseInt(int i){
    unsigned char c1, c2, c3, c4;
    c1 = i & 255;
    c2 = (i >> 8) & 255;
    c3 = (i >> 16)& 255;
    c4 = (i >> 24)& 255;

    return ((int)c1<<24)+((int)c2<<16)+((int)c3<<8)+c4;
}

static int IsAllocationBlockUsed(UInt32 thisAllocationBlock, UInt8* allocationFileContents)
{
    UInt8 thisByte;

    thisByte = allocationFileContents[thisAllocationBlock / 8];
    //log_mesg(0, 0, 0, fs_opt.debug, "IsAB:%i\n", (thisByte & (1 << (7 - (thisAllocationBlock % 8)))));
    return (thisByte & (1 << (7 - (thisAllocationBlock % 8)))) != 0;
}

static void print_fork_data(HFSPlusForkData* fork){
    int i = 0;

    HFSPlusExtentDescriptor* exten;
    log_mesg(2, 0, 0, fs_opt.debug, "%s: logicalSize: %#lx\n", __FILE__, fork->logicalSize);
    log_mesg(2, 0, 0, fs_opt.debug, "%s: clumpSize: %i\n", __FILE__, reverseInt(fork->clumpSize));
    log_mesg(2, 0, 0, fs_opt.debug, "%s: totalBlocks: %i\n", __FILE__, reverseInt(fork->totalBlocks));
    for (i = 0; i < 8; i++ ){
        exten = &fork->extents[i];
        log_mesg(2, 0, 0, fs_opt.debug, "%s: \texten %i startBlock: %i\n", __FILE__, i, reverseInt(exten->startBlock));
        log_mesg(2, 0, 0, fs_opt.debug, "%s: \texten %i blockCount: %i\n", __FILE__, i, reverseInt(fork->extents[i].blockCount));

    }
}

/// open device
static void fs_open(char* device){

    int s, r;
    char *buffer;
    short HFS_Version;
    char HFS_Signature[2];
    int HFS_Clean = 0;

    ret = open(device, O_RDONLY);
    s = lseek(ret, 1024, SEEK_SET);
    buffer = (char*)malloc(sizeof(HFSPlusVolumeHeader));
    r = read (ret, buffer, sizeof(HFSPlusVolumeHeader));
    memcpy(&sb, buffer, sizeof(HFSPlusVolumeHeader));

    HFS_Signature[0] = (char)sb.signature;
    HFS_Signature[1] = (char)(sb.signature>>8);
    HFS_Version = (short)reverseShort(sb.version);
    HFS_Clean = (reverseInt(sb.attributes)>>8) & 1;

    log_mesg(3, 0, 0, fs_opt.debug, "%s: Signature=%c%c\n", __FILE__, HFS_Signature[0], HFS_Signature[1]);
    log_mesg(3, 0, 0, fs_opt.debug, "%s: Version=%i\n", __FILE__, HFS_Version);
    log_mesg(3, 0, 0, fs_opt.debug, "%s: Attr-Unmounted=%i(1 is clean, 0 is dirty)\n", __FILE__, HFS_Clean);
    log_mesg(3, 0, 0, fs_opt.debug, "%s: Attr-Inconsistent=%i\n", __FILE__, (reverseInt(sb.attributes)>>11) & 1);

    if(fs_opt.ignore_fschk){
        log_mesg(1, 0, 0, fs_opt.debug, "%s: Ignore filesystem check\n", __FILE__);
    } else {
        if (HFS_Clean)
            log_mesg(3, 0, 0, fs_opt.debug, "%s: HFS_Plus '%s' is clean\n", __FILE__, device);
        else 
            log_mesg(0, 1, 1, fs_opt.debug, "%s: HFS_Plus Volume '%s' is scheduled for a check or it was shutdown\nuncleanly. Please fix it by fsck.\n", __FILE__, device);
    }

    free(buffer);

}

static void fs_close(){

    close(ret);

}

extern void readbitmap(char* device, image_head image_hdr, char* bitmap, int pui){

    int r, IsUsed = 0, s, i;
    UInt8 *buffer2;
    long int tb = 0, rb = 0, bused = 0, bfree = 0;
    UInt32 b;
    int start = 0;
    int bit_size = 1;
    int allocation_exten = 0;
    long int allocation_start_block;
    long int allocation_block_size;
    long int exten_bitmap = 0;


    fs_open(device);
    tb = reverseInt((int)sb.totalBlocks);
    rb = (tb/8)+1;

    /// init progress
    progress_bar   prog;	/// progress_bar structure defined in progress.h
    progress_init(&prog, start, image_hdr.totalblock, bit_size);

    for (i = 0; i < tb; i++)
        bitmap[i] = 1;

    for (allocation_exten = 0; allocation_exten <= 7; allocation_exten++){
        allocation_start_block = 4096*reverseInt(sb.allocationFile.extents[allocation_exten].startBlock);

        allocation_block_size = 4096*reverseInt(sb.allocationFile.extents[allocation_exten].blockCount);
        if((allocation_start_block == 0) && (allocation_block_size == 0)){
            continue;
        }

        s = lseek(ret, allocation_start_block, SEEK_SET);
        rb = allocation_block_size;
        buffer2 = (UInt8*)malloc(rb);
        r = read(ret, buffer2, rb);
        for(b = exten_bitmap ; (b < allocation_block_size*8) && (b < tb); b++){
            int check_block = b;
            IsUsed = IsAllocationBlockUsed(check_block, buffer2);
            if (IsUsed){
                bused++;
                bitmap[b] = 1;
                log_mesg(3, 0, 0, fs_opt.debug, "%s: used b = %i\n", __FILE__, b);
            } else {
                bfree++;
                bitmap[b] = 0;
                log_mesg(3, 0, 0, fs_opt.debug, "%s: free b = %i\n", __FILE__, b);
            }
            exten_bitmap = allocation_block_size*8;
            /// update progress
            update_pui(&prog, b, 0);

        }
        free(buffer2);
        log_mesg(2, 0, 0, 2, "%s: buffer2:%i\n", __FILE__, buffer2);

        log_mesg(2, 0, 0, 2, "%s: bfree:%i\n", __FILE__, bfree);
        log_mesg(2, 0, 0, 2, "%s: bused:%i\n", __FILE__, bused);
    }
    if(bused != (reverseInt(sb.totalBlocks) - reverseInt(sb.freeBlocks)))
        log_mesg(0, 1, 1, fs_opt.debug, "%s: bitmap count error, used:%i\n", __FILE__, bused);

    fs_close();
    /// update progress
    update_pui(&prog, 1, 1);
}

extern void initial_image_hdr(char* device, image_head* image_hdr)
{

    fs_open(device);
    memcpy(image_hdr->magic, IMAGE_MAGIC, IMAGE_MAGIC_SIZE);
    memcpy(image_hdr->fs, hfsplus_MAGIC, FS_MAGIC_SIZE);
    image_hdr->block_size  = (int)reverseInt(sb.blockSize);
    image_hdr->totalblock  = (unsigned long long)reverseInt(sb.totalBlocks);
    image_hdr->device_size = (unsigned long long)(image_hdr->block_size * image_hdr->totalblock);
    image_hdr->usedblocks  = (unsigned long long)(reverseInt(sb.totalBlocks) - reverseInt(sb.freeBlocks));
    log_mesg(2, 0, 0, 2, "%s: blockSize:%i\n", __FILE__, reverseInt(sb.blockSize));
    log_mesg(2, 0, 0, 2, "%s: totalBlocks:%i\n", __FILE__, reverseInt(sb.totalBlocks));
    log_mesg(2, 0, 0, 2, "%s: freeBlocks:%i\n", __FILE__, reverseInt(sb.freeBlocks));
    print_fork_data(&sb.allocationFile);
    print_fork_data(&sb.extentsFile);
    print_fork_data(&sb.catalogFile);
    print_fork_data(&sb.attributesFile);
    print_fork_data(&sb.startupFile);
    fs_close();
}
