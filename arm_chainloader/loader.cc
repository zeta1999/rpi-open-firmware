/*=============================================================================
Copyright (C) 2016-2017 Authors of rpi-open-firmware
All rights reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

FILE DESCRIPTION
Second stage bootloader.

=============================================================================*/

#include <drivers/fatfs/ff.h>
#include <chainloader.h>
#include <drivers/mailbox.hpp>
#include <drivers/block_device.hpp>
#include <libfdt.h>
#include <memory_map.h>
#include <hardware.h>

#include <string.h>
#include <stdio.h>

#define logf(fmt, ...) print_timestamp(); printf("[LDR:%s]: " fmt, __FUNCTION__, ##__VA_ARGS__);

FATFS g_BootVolumeFs;

#define ROOT_VOLUME_PREFIX "0:"
#define DTB_LOAD_ADDRESS    0xF000000 // 240mb from start
#define KERNEL_LOAD_ADDRESS 0x2000000 // 32mb from start
#define INITRD_LOAD_ADDRESS 0x4000000 // 64mb from start

extern "C" void disable_cache();

typedef void (*linux_t)(uint32_t, uint32_t, void*);

struct mem_entry {
  uint32_t address;
  uint32_t size;
};

static_assert((MEM_USABLE_START+0x800000) < KERNEL_LOAD_ADDRESS,
              "memory layout would not allow for kernel to be loaded at KERNEL_LOAD_ADDRESS, please check memory_map.h");

uint32_t htonl(uint32_t hostlong) {
  return ((hostlong & 0x000000ff) << 24) |
         ((hostlong & 0x0000ff00) << 8) |
         ((hostlong & 0x00ff0000) >> 8) |
         ((hostlong & 0xff000000) >> 24);
}

void hex32(uint32_t input, char *output) {
  output[8] = 0;
  for (int i=7; i>=0; i--) {
    int nibble = input & 0xf;
    if (nibble <= 9) {
      output[i] = 0x30 + nibble;
    } else {
      output[i] = 0x57 + nibble;
    }
    input = input >> 4;
  }
}

struct LoaderImpl {
	inline bool file_exists(const char* path) {
		return f_stat(path, NULL) == FR_OK;
	}

	size_t read_file(const char* path, uint8_t*& dest, bool should_alloc = true) {
		/* ensure file exists first */
		if(!file_exists(path))
			panic("attempted to read %s, but it does not exist", path);
                uint32_t start = ST_CLO;

		/* read entire file into buffer */
		FIL fp;
		f_open(&fp, path, FA_READ);

		unsigned int len = f_size(&fp);

		if(should_alloc) {
			/*
			 * since this can be used for strings, there's no harm in reserving an
			 * extra byte for the null terminator and appending it.
			 */
			uint8_t* buffer = new uint8_t[len + 1];
			dest = buffer;
			buffer[len] = 0;
		}

		logf("%s: reading %d bytes to 0x%X ~%dmb (allocated=%d) ...\n", path, len, (unsigned int)dest, ((unsigned int)dest)/1024/1024, should_alloc);

		f_read(&fp, dest, len, &len);
		f_close(&fp);

                uint32_t stop = ST_CLO;
                uint32_t elapsed = stop - start;

                uint32_t bytes_per_second = (double)len / ((double)(elapsed) / 1000 / 1000);
                printf("%d kbyte copied at a rate of %ld kbytes/second\n", len/1024, bytes_per_second/1024);

		return len;
	}

  uint8_t* load_fdt(const char* filename, const char* cmdline, uint8_t *initrd_start, size_t initrd_size) {
    /* read device tree blob */
    uint8_t* fdt = reinterpret_cast<uint8_t*>(DTB_LOAD_ADDRESS);
    size_t sz = read_file(filename, fdt, false);
    logf("FDT loaded at %p, size is %d\n", fdt, sz);

    void* v_fdt = reinterpret_cast<void*>(fdt);

    int res;

    if ((res = fdt_check_header(v_fdt)) != 0) {
      panic("fdt blob invalid, fdt_check_header returned %d", res);
    }

    res = fdt_open_into(v_fdt, v_fdt, sz + 4096);

    char serial_number[9];
    hex32(g_FirmwareData.serial, serial_number);
    // /serial-number is an ascii string containing the serial#
    res = fdt_setprop(v_fdt, 0, "serial-number", serial_number, 9);
    // /system/linux,serial is an 8byte raw serial#

    int node = fdt_path_offset(v_fdt, "ethernet0");
    if (node < 0) {
      panic("cant find ethernet0");
    } else {
      uint32_t serial = g_FirmwareData.serial;
      uint8_t mac[] = { 0xb8
                      , 0x27
                      , 0xeb
                      , (uint8_t)((serial >> 16) & 0xff)
                      , (uint8_t)((serial >> 8) & 0xff)
                      , (uint8_t)(serial & 0xff) };
      res = fdt_setprop(v_fdt, node, "local-mac-address", mac, 6);
    }

    node = fdt_path_offset(v_fdt, "/system");
    if (node < 0) {
      panic("cant find /system");
    } else {
      uint32_t revision = htonl(g_FirmwareData.revision);
      res = fdt_setprop(v_fdt, node, "linux,revision", &revision, 4);
    }

    node = fdt_path_offset(v_fdt, "/chosen");
    if (node < 0) panic("no chosen node in fdt");

    /* pass in command line args */
    res = fdt_setprop(v_fdt, node, "bootargs", cmdline, strlen((char*) cmdline) + 1);
    // chosen.txt describes linux,initrd-start and linux,initrd-end within the chosen node
    uint32_t value = htonl((uint32_t)initrd_start);
    res = fdt_setprop(v_fdt, node, "linux,initrd-start", &value, 4);
    uint32_t initrd_end = htonl((uint32_t)(initrd_start + initrd_size));
    res = fdt_setprop(v_fdt, node, "linux,initrd-end", &initrd_end, 4);

    /* pass in a memory map, skipping first meg for bootcode */
    int memory = fdt_path_offset(v_fdt, "/memory");
    if(memory < 0) panic("no memory node in fdt");

    /* start the memory map at 1M/16 and grow continuous for 256M
     * TODO: does this disrupt I/O? */

    char dtype[] = "memory";
    struct mem_entry memmap[] = {
      { .address = htonl(1024 * 128), .size = htonl(((256*3) * 1024 * 1024) - (1024 * 128)) }
    };
    res = fdt_setprop(v_fdt, memory, "reg", (void*) memmap, sizeof(memmap));

    logf("(valid) fdt loaded at 0x%p\n", fdt);

    return fdt;
  }

  void teardown_hardware() {
    BlockDevice* bd = get_sdhost_device();
    if (bd)
      bd->stop();
  }

  const char *detect_model_dtb() {
    // currently, this detects purely based on the arm model
    // in future, it should get it from the full board revision, via OTP
    uint32_t arm_cpuid;
    // read MIDR reg
    __asm__("mrc p15, 0, %0, c0, c0, 0" : "=r"(arm_cpuid));
    // from https://github.com/dwelch67/raspberrypi/blob/master/boards/cpuid/cpuid.c
    switch (arm_cpuid) {
    case 0x410FB767:
      return "rpi1.dtb";
      break;
    case 0x410FC075:
      return "rpi2.dtb";
      break;
    case 0x410FD034:
      return "rpi3.dtb";
      break;
    // 410FD083 is cortex A72, rpi4
    default:
      logf("unknown rpi model, cpuid is 0x%lx\n", arm_cpuid);
      return "unknown.dtb";
    }
  }

	LoaderImpl() {
		logf("Mounting boot partitiion ...\n");
		FRESULT r = f_mount(&g_BootVolumeFs, ROOT_VOLUME_PREFIX, 1);
		if (r != FR_OK) {
			panic("failed to mount boot partition, error: %d", (int)r);
		}
		logf("Boot partition mounted!\n");


		/* read the kernel as a function pointer at fixed address */
		uint8_t* zImage = reinterpret_cast<uint8_t*>(KERNEL_LOAD_ADDRESS);
		linux_t kernel = reinterpret_cast<linux_t>(zImage);

		size_t ksize = read_file("zImage", zImage, false);
		logf("zImage loaded at 0x%X\n", (unsigned int)kernel);

    uint8_t *initrd = reinterpret_cast<uint8_t*>(INITRD_LOAD_ADDRESS);
#if 0
    uint32_t *initrd32 = reinterpret_cast<uint32_t*>(INITRD_LOAD_ADDRESS);
    for (int i=0; i < (1024 * 1024 * 16); i++) {
      initrd32[i] = i;
    }
    logf("pattern loaded\n");
    udelay(1000 * 1000 * 600);
    logf("checking pattern\n");
    for (int i=0; i < (1024 * 1024 * 16); i++) {
      if (initrd32[i] != i) {
        printf("mismatch at 0x%x, 0x%lx\n", i, initrd32[i]);
        panic("memory error");
      }
    }
    logf("memtest done\n");
    panic("stopping");
#endif
    size_t initrd_size = read_file("initrd", initrd, false);

    /* read the command-line null-terminated */
    uint8_t *cmdline;
    size_t cmdlen = read_file("cmdline.txt", cmdline);

    logf("kernel cmdline: %s\n", cmdline);

    const char *dtb_name = detect_model_dtb();

    logf("using %s\n", dtb_name);

      /* load flat device tree */
    uint8_t* fdt = load_fdt(dtb_name, (char*)cmdline, initrd, initrd_size);

    /* once the fdt contains the cmdline, it is not needed */
    delete[] cmdline;

    /* flush the cache */
    logf("Flushing....\n");
    for (uint8_t* i = zImage; i < zImage + ksize; i += 32) {
      __asm__ __volatile__ ("mcr p15,0,%0,c7,c10,1" : : "r" (i) : "memory");
    }

    /* the eMMC card in particular needs to be reset */
    teardown_hardware();
    disable_cache();

    /* fire away -- this should never return */
    logf("Jumping to the Linux kernel...\n");
    kernel(0, ~0, fdt);
  }
};

static LoaderImpl STATIC_APP g_Loader {};
