/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* TODO:
 *       1) Perform substitution of the kernel command-line fields with the
 *          appropriate values based on bvb_boot_image_header.h
 */

#include <efi.h>
#include <efilib.h>
#include "bub_boot_kernel.h"
#include "bub_sysdeps.h"

// GPT related constants
#define GPT_REVISION 0x00010000
#define GPT_MAGIC "EFI PART"
#define GPT_MIN_SIZE 92
#define GPT_ENTRIES_LBA 2
#define BLOCK_SIZE 512
#define ENTRIES_PER_BLOCK 4
#define ENTRY_NAME_LEN 36
#define MAX_GPT_ENTRIES 128

typedef struct {
  UINT8   signature[8];
  UINT32  revision;
  UINT32  header_size;
  UINT32  header_crc32;
  UINT32  reserved;
  UINT64  header_lba;
  UINT64  alternate_header_lba;
  UINT64  first_usable_lba;
  UINT64  last_usable_lba;
  UINT8   disk_guid[16];
  UINT64  entry_lba;
  UINT32  entry_count;
  UINT32  entry_size;
  UINT32  entry_crc32;
  UINT8   reserved2[420];
} GPTHeader;

typedef struct {
  UINT8   type_GUID[16];
  UINT8   unique_GUID[16];
  UINT64  first_lba;
  UINT64  last_lba;
  UINT64  flags;
  CHAR16  name[ENTRY_NAME_LEN];  // UTF-16LE encoding, NULL terminated
} GPTEntry;

/*
 * Note: The below header definitions are taken from
 *       system/core/mkbootimg/bootimg.h
 */
typedef struct boot_img_hdr boot_img_hdr;

#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024

struct boot_img_hdr
{
    UINT8 magic[BOOT_MAGIC_SIZE];

    UINT32 kernel_size;  /* size in bytes */
    UINT32 kernel_addr;  /* physical load addr */

    UINT32 ramdisk_size; /* size in bytes */
    UINT32 ramdisk_addr; /* physical load addr */

    UINT32 second_size;  /* size in bytes */
    UINT32 second_addr;  /* physical load addr */

    UINT32 tags_addr;    /* physical addr for kernel tags */
    UINT32 page_size;    /* flash page size we assume */
    UINT32 unused;       /* reserved for future expansion: MUST be 0 */

    /* operating system version and security patch level; for
     * version "A.B.C" and patch level "Y-M-D":
     * ver = A << 14 | B << 7 | C         (7 bits for each of A, B, C)
     * lvl = ((Y - 2000) & 127) << 4 | M  (7 bits for Y, 4 bits for M)
     * os_version = ver << 11 | lvl */
    UINT32 os_version;

    UINT8 name[BOOT_NAME_SIZE]; /* asciiz product name */

    UINT8 cmdline[BOOT_ARGS_SIZE];

    UINT32 id[8]; /* timestamp / checksum / sha1 / etc */

    /* Supplemental command line data; kept here to maintain
     * binary compatibility with older versions of mkbootimg */
    UINT8 extra_cmdline[BOOT_EXTRA_ARGS_SIZE];
} __attribute__((packed));

/*
** +-----------------+
** | boot header     | 1 page
** +-----------------+
** | kernel          | n pages
** +-----------------+
** | ramdisk         | m pages
** +-----------------+
** | second stage    | o pages
** +-----------------+
**
** n = (kernel_size + page_size - 1) / page_size
** m = (ramdisk_size + page_size - 1) / page_size
** o = (second_size + page_size - 1) / page_size
**
** 0. all entities are page_size aligned in flash
** 1. kernel and ramdisk are required (size != 0)
** 2. second is optional (second_size == 0 -> no second)
** 3. load each element (kernel, ramdisk, second) at
**    the specified physical address (kernel_addr, etc)
** 4. prepare tags at tag_addr.  kernel_args[] is
**    appended to the kernel commandline in the tags.
** 5. r0 = 0, r1 = MACHINE_TYPE, r2 = tags_addr
** 6. if second_size != 0: jump to second_addr
**    else: jump to kernel_addr
*/

#define IMG_SIZE(entry, block) \
  (entry->last_lba - entry->first_lba) * block->Media->BlockSize

#define SIZE_BLOCK_ALIGN(bytes, block_size) \
  ((bytes + block_size - 1) / block_size) * block_size

#define OFFSET_BLOCK_ALIGN(bytes, block_size) \
  (bytes / block_size) * block_size

/* uefi_call_wrapper's second arguments is the number of argumets for the
 * called function
 */
#define NUM_ARGS_HANDLE_PROTOCOL 3
#define NUM_ARGS_ALLOCATE_POOL 3
#define NUM_ARGS_LOCATE_DEVICE_PATH 3
#define NUM_ARGS_READ_BLOCKS 5
#define NUM_ARGS_LOAD_IMAGE 6

/* Helper method to get the parent path to the current |walker| path given the
 * initial path, |init|. Resulting path is stored in |next|.  Caller is
 * responsible for freeing |next|.
 *
 * @return EFI_STATUS Standard UEFI error code, EFI_SUCCESS on success.
 */
static EFI_STATUS walk_path(IN EFI_DEVICE_PATH *init,
                            IN EFI_DEVICE_PATH *walker,
                            OUT EFI_DEVICE_PATH **next) {
  EFI_STATUS err;

  // Number of bytes from initial path to current walker.
  UINTN walker_bytes = (UINT8 *)NextDevicePathNode(walker) - (UINT8 *)init;

  *next = (EFI_DEVICE_PATH*)bub_malloc_(sizeof(EFI_DEVICE_PATH) + walker_bytes);
  if (*next == NULL)
    return EFI_NOT_FOUND;

  // Copy in the previous paths.
  bub_memcpy((*next), init, walker_bytes);
  // Copy in the new ending of the path.
  bub_memcpy((UINT8 *)(*next) + walker_bytes,
             EndDevicePath,
             sizeof(EFI_DEVICE_PATH));
  return EFI_SUCCESS;
}

/* Helper method to validate a GPT header, |gpth|.
 *
 * @return EFI_STATUS EFI_SUCCESS on success.
 */
static EFI_STATUS validate_gpt(const IN GPTHeader *gpth) {
  if (bub_memcmp(gpth->signature, GPT_MAGIC, sizeof(gpth->signature))
      != 0) {
    bub_warning("GPT signature does not match.\n");
    return EFI_NOT_FOUND;
  }
  // Make sure GPT header bytes are within minimun and block size.
  if (gpth->header_size < GPT_MIN_SIZE) {
    bub_warning("GPT header too small.\n");
    return EFI_NOT_FOUND;
  }
  if (gpth->header_size > BLOCK_SIZE) {
    bub_warning("GPT header too big.\n");
    return EFI_NOT_FOUND;
  }

  GPTHeader gpth_tmp = {0};
  bub_memcpy(&gpth_tmp, gpth, sizeof(GPTHeader));
  UINT32 gpt_header_crc = gpth_tmp.header_crc32;
  gpth_tmp.header_crc32 = 0;
  UINT32 gpt_header_crc_calc =
    CalculateCrc((UINT8*)&gpth_tmp, gpth_tmp.header_size);

#ifdef BUB_ENABLE_DEBUG
  Print(L"GPT Header CRC: %d\n", gpt_header_crc);
  Print(L"GPT Header CRC calculated: %d\n", gpt_header_crc_calc);
#endif
  if(gpt_header_crc != gpt_header_crc_calc) {
    bub_warning("GPT header crc invalid.\n");
    return EFI_NOT_FOUND;
  }

  if (gpth->revision != GPT_REVISION) {
    bub_warning("GPT header wrong revision.\n");
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/* Looks through |block_io| to search for a GPT entry named |partition_name|, a
 * NUULL-terminated string. Allocates a GPTEntry struct for |entry_buf|. Caller
 * is responsible for freeing |entry_buf|.
 *
 * @return EFI_NOT_FOUND on error, EFI SUCCESS on success.
 */
static EFI_STATUS PartitionEntryByName(IN EFI_BLOCK_IO *block_io,
                                       const char* partition_name,
                                       GPTEntry** entry_buf) {
  EFI_STATUS err;
  GPTHeader* gpt_header = NULL;
  GPTEntry all_gpt_entries[MAX_GPT_ENTRIES];
  CHAR16* partition_name_ucs2 = NULL;
  UINTN partition_name_bytes;

  gpt_header = (GPTHeader*)bub_malloc_(sizeof(GPTHeader));
  if (gpt_header == NULL) {
    bub_warning("Could not allocate for GPT header\n");
    return EFI_NOT_FOUND;
  }

  *entry_buf = (GPTEntry*)bub_malloc_(sizeof(GPTEntry) * ENTRIES_PER_BLOCK);
  if (entry_buf == NULL) {
    bub_warning("Could not allocate for partition entry\n");
    bub_free(gpt_header);
    return EFI_NOT_FOUND;
  }

  err = uefi_call_wrapper(block_io->ReadBlocks, NUM_ARGS_READ_BLOCKS,
                          block_io,
                          block_io->Media->MediaId,
                          1,
                          sizeof(GPTHeader),
                          gpt_header);
  if (EFI_ERROR(err)) {
    bub_warning("Could not ReadBlocks for gpt header\n");
    bub_free(gpt_header);
    return EFI_NOT_FOUND;
  }

  partition_name_bytes = bub_strlen(partition_name) + 1;
  partition_name_ucs2 =
    bub_calloc(sizeof(CHAR16) * partition_name_bytes);
  if (partition_name_ucs2 == NULL) {
    bub_warning ("Could not allocate for ucs2 partition name\n");
    bub_free(gpt_header);
    return EFI_NOT_FOUND;
  }
  if (utf8_to_ucs2(partition_name,
                   partition_name_bytes,
                   partition_name_ucs2,
                   sizeof(CHAR16) * partition_name_bytes)) {
    bub_warning("Could not convert partition name to UCS-2\n");
    bub_free(gpt_header);
    bub_free(partition_name_ucs2);
    return EFI_NOT_FOUND;
  }

#ifdef BUB_ENABLE_DEBUG
  Print(L"\nENTRY: %d, BLOCKSIZE: %d, GPTEntry: %d\n",
            gpt_header->entry_count,
            block_io->Media->BlockSize,
            sizeof(GPTEntry));
#endif

  // Block-aligned bytes for entries.
  UINTN entries_num_bytes = block_io->Media->BlockSize *
                            (MAX_GPT_ENTRIES / ENTRIES_PER_BLOCK);

  err = uefi_call_wrapper(block_io->ReadBlocks, NUM_ARGS_READ_BLOCKS,
                          block_io,
                          block_io->Media->MediaId,
                          GPT_ENTRIES_LBA,
                          entries_num_bytes,
                          &all_gpt_entries);
  if (EFI_ERROR(err)) {
    bub_warning("Could not ReadBlocks for GPT header\n");
    bub_free(gpt_header);
    bub_free(partition_name_ucs2);
    return EFI_NOT_FOUND;
  }

  // Find matching partition name.
  UINT8 i;
  for (i = 0; i < gpt_header->entry_count; ++i)
    if (!bub_memcmp(all_gpt_entries[i].name,
                    partition_name_ucs2,
                    sizeof(CHAR16) * bub_strlen(partition_name))) {
#ifdef BUB_ENABLE_DEBUG
      Print(L"Boot Partition Name is: %s\n", all_gpt_entries[i].name);
      Print(L"Boot Partition LBA is: %d\n", all_gpt_entries[i].first_lba);
#endif
      bub_memcpy((*entry_buf), &all_gpt_entries[i], sizeof(GPTEntry));
      bub_free(partition_name_ucs2);
      bub_free(gpt_header);
      return EFI_SUCCESS;
    }

  bub_free(partition_name_ucs2);
  bub_free(gpt_header);
  return EFI_NOT_FOUND;
}

/* Allocates a pool of memory in the EfiLoaderData region for the LoadOptions
 * member of |loaded_image|.  The LoadOptions member is needed by next boot
 * stage. In the case of Linux kernel images, the EFI_STUB is this stage. The
 * stub is expected to pass the stored string (CHAR16 type) to the kernel.
 *
 * |image_header| is assumed to contain the kernel command line string.
 * |image| is the kernel image handle to be booted.
 *
 *
 * @return EFI_STATUS EFI_NOT_FOUND on fail. EFI_SUCCESS on success.
 *
 * TODO: Perform substitution of the kernel command-line fields with the
 *       appropriate values based on bvb_boot_image_header.h here.
 */
static EFI_STATUS LoadParameters(EFI_HANDLE image,
                                 boot_img_hdr *image_header,
                                 EFI_LOADED_IMAGE **loaded_image) {
  EFI_STATUS err;
  EFI_GUID loaded_image_protocol = LOADED_IMAGE_PROTOCOL;
  err = uefi_call_wrapper(BS->HandleProtocol, NUM_ARGS_HANDLE_PROTOCOL,
                          image,
                          &loaded_image_protocol,
                          (VOID **) &(*loaded_image));
  if (EFI_ERROR(err)) {
    bub_warning("Could not get loaded boot image\n");
    return EFI_NOT_FOUND;
  }

  err = uefi_call_wrapper(BS->AllocatePool, NUM_ARGS_ALLOCATE_POOL,
                          EfiLoaderData,
                          BOOT_ARGS_SIZE * sizeof(UINT16),
                          &(*loaded_image)->LoadOptions);
  if (EFI_ERROR(err)) {
    bub_warning("Could not allocate for kernel parameters\n");
    return EFI_NOT_FOUND;
  }

  UINT32 i;
  for (i = 0; image_header->cmdline[i] != '\0'; ++i)
    ((CHAR16 *)((*loaded_image)->LoadOptions))[i] =
      (CHAR16)image_header->cmdline[i];
  ((CHAR16 *)((*loaded_image)->LoadOptions))[i] = L'\0';

  (*loaded_image)->LoadOptionsSize = i * sizeof(UINT16);

  return EFI_SUCCESS;
}

/* Queries |disk_handle| for a |block_io| device and the corresponding path,
 * |block_path|.  The |block_io| device is found by iteratively querying parent
 * devices and checking for a GPT Header.  This ensures the resulting
 * |block_io| device is the top level block device having access to partition
 * entries.
 *
 * @return EFI_STATUS EFI_NOT_FOUND on fail, EFI_SUCCESS otherwise.
 */
static EFI_STATUS getDiskBlockIO(IN EFI_HANDLE* disk_handle,
                                 OUT EFI_BLOCK_IO** block_io,
                                 OUT EFI_DEVICE_PATH** block_path) {
  EFI_STATUS err;
  EFI_DEVICE_PATH *walker_path;
  EFI_DEVICE_PATH *init_path;
  GPTHeader gpt_header = {{0}};
  init_path = DevicePathFromHandle(disk_handle);

#ifdef BUB_ENABLE_DEBUG
  Print(L"Initial Device Path: %s\n", DevicePathToStr(init_path));
#endif

  if (!init_path)
    return EFI_NOT_FOUND;

  walker_path = init_path;
  while(!IsDevicePathEnd(walker_path)) {
    walker_path = NextDevicePathNode(walker_path);

#ifdef BUB_ENABLE_DEBUG
    Print(L"DevicePathType: %x\n", DevicePathType(walker_path));
#endif

    err = walk_path(init_path, walker_path, &(*block_path));
    if (EFI_ERROR(err)) {
      bub_warning("Cannot walk device path.\n");
      return EFI_NOT_FOUND;
    }

#ifdef BUB_ENABLE_DEBUG
    Print(L"Walking Device Path   : %s\n", DevicePathToStr(*block_path));
#endif
    err = uefi_call_wrapper(BS->LocateDevicePath, NUM_ARGS_LOCATE_DEVICE_PATH,
                            &BlockIoProtocol,
                            &(*block_path),
                            &disk_handle);
    if (EFI_ERROR(err)) {
      bub_warning("LocateDevicePath, BLOCK_IO_PROTOCOL.\n");
      bub_free(*block_path);
      continue;
    }

    // Handle Block i/o
    // Attempt to get handle on device, must be BlockIo type.
    err = uefi_call_wrapper(BS->HandleProtocol, NUM_ARGS_HANDLE_PROTOCOL,
                            disk_handle,
                            &BlockIoProtocol,
                            (VOID **)&(*block_io));
    if (EFI_ERROR(err)) {
      bub_warning("HandleProtocol, BLOCK_IO_PROTOCOL.\n");
      bub_free(*block_path);
      continue;
    }

    if ((*block_io)->Media->LogicalPartition ||
        !(*block_io)->Media->MediaPresent) {
      bub_warning("Logical partion or No Media Present, continue...\n");
      bub_free(*block_path);
      continue;
    }

    err = uefi_call_wrapper((*block_io)->ReadBlocks, NUM_ARGS_READ_BLOCKS,
                            (*block_io),
                            (*block_io)->Media->MediaId,
                            1,
                            sizeof(GPTHeader),
                            &gpt_header);

    if (EFI_ERROR(err)) {
      bub_warning("ReadBlocks, Block Media error.\n");
      bub_free(*block_path);
      continue;
    }

    err = validate_gpt(&gpt_header);
    if (EFI_ERROR(err)) {
      bub_warning("Invalid GPTHeader\n");
      bub_free(*block_path);
      continue;
    }

#ifdef BUB_ENABLE_DEBUG
    Print(L"Validated GPT\n");
#endif
    return EFI_SUCCESS;
  }

  (*block_io) = NULL;
  return EFI_NOT_FOUND;
}

int bub_boot_kernel(EFI_HANDLE app_image, const char* boot_partition_name) {
  EFI_STATUS err;
  EFI_LOADED_IMAGE *loaded_app_image = NULL;
  EFI_GUID loaded_image_protocol = LOADED_IMAGE_PROTOCOL;
  EFI_BLOCK_IO* block_io;
  EFI_DEVICE_PATH* block_io_path;
  GPTEntry* partition_entry;
  UINT8* boot_buf = NULL;
  UINT8* kernel_buf = NULL;
  boot_img_hdr* head_buf = NULL;
  EFI_HANDLE kernel_image;
  EFI_LOADED_IMAGE *loaded_kernel_image = NULL;

  err = uefi_call_wrapper(BS->HandleProtocol, NUM_ARGS_HANDLE_PROTOCOL,
                          app_image,
                          &loaded_image_protocol,
                          (VOID **) &loaded_app_image);
  if (EFI_ERROR(err)) {
    bub_warning("HandleProtocol, LOADED_IMAGE_PROTOCOL.\n");
    return 1;
  }

#ifdef BUB_ENABLE_DEBUG
  Print(L"Image base        : %lx\n", loaded_app_image->ImageBase);
  Print(L"Image size        : %lx\n", loaded_app_image->ImageSize);
  Print(L"Image file        : %s\n",
            DevicePathToStr(loaded_app_image->FilePath));
  Print(L"Path Type         : %x\n", loaded_app_image->FilePath->Type);
  Print(L"Path Sub-type     : %x\n", loaded_app_image->FilePath->SubType);
  Print(L"LoadOptionsSize   : %d\n", loaded_app_image->LoadOptionsSize);
#endif

  // Get parent device block i/o.
  err = getDiskBlockIO(loaded_app_image->DeviceHandle,
                       &block_io,
                       &block_io_path);
  if (EFI_ERROR(err)) {
    bub_warning("Could not block device handle.\n");
    return 1;
  }


  // Get lba of partition based on name.
  err = PartitionEntryByName(block_io, boot_partition_name ,&partition_entry);
  if (EFI_ERROR(err)) {
    bub_warning("Could not find Image LBA offset.\n");
    return 1;
  }

#ifdef BUB_ENABLE_DEBUG
  Print(L"Block IO media block size: %d\n", block_io->Media->BlockSize);
  Print(L"Kernel Image LBA: 0x%x\n", partition_entry->first_lba);
  Print(L"Kernel size:  0x%x\n", IMG_SIZE(partition_entry,block_io));
#endif

  boot_buf = (UINT8*)bub_malloc_(IMG_SIZE(partition_entry, block_io));
  if (boot_buf == NULL) {
    bub_warning("Could not allocate for boot buffer.\n");
    return 1;
  }

#ifdef BUB_ENABLE_DEBUG
  Print(L"Loading Boot Image...\n");
#endif

  err = uefi_call_wrapper(block_io->ReadBlocks, NUM_ARGS_READ_BLOCKS,
                          block_io,
                          block_io->Media->MediaId,
                          partition_entry->first_lba,
                          IMG_SIZE(partition_entry, block_io),
                          boot_buf);

  if (EFI_ERROR(err)) {
    bub_warning("Could not read load partition image.\n");
    return 1;
  }

  head_buf = (boot_img_hdr *)boot_buf;
  err = uefi_call_wrapper(BS->AllocatePool, NUM_ARGS_ALLOCATE_POOL,
                          EfiLoaderCode,
                          head_buf->kernel_size,
                          &kernel_buf);
  if (EFI_ERROR(err)) {
    bub_warning("Could not allocate for kernel buffer.\n");
    return 1;
  }

#ifdef BUB_ENABLE_DEBUG
  // Print Header info
  Print(L"kernel size:      %x\n", head_buf->kernel_size);
  Print(L"kernel_addr:        %x\n", head_buf->kernel_addr);
  Print(L"ramdisk_size:       %x\n", head_buf->ramdisk_size);
  Print(L"ramdisk_addr:       %x\n", head_buf->ramdisk_addr);
  Print(L"second_size:        %x\n", head_buf->second_size);
  Print(L"second_addr:        %x\n", head_buf->second_addr);
  Print(L"tags_addr:          %x\n", head_buf->tags_addr);
  Print(L"page_size:          %x\n", head_buf->page_size);
  Print(L"os_version:         %x\n", head_buf->os_version);
#endif

  // Checks on buffer overflow.
  if (head_buf->kernel_size > IMG_SIZE(partition_entry, block_io)) {
    bub_warning("Kernel size beyond allowed boundary.\n");
    return 1;
  }
  if (head_buf->page_size >
      (IMG_SIZE(partition_entry, block_io) - sizeof(boot_img_hdr))) {
    bub_warning("Page size too big.\n");
    return 1;
  }

  bub_memcpy(kernel_buf,
             boot_buf + head_buf->page_size,
             head_buf->kernel_size);

  err = uefi_call_wrapper(BS->LoadImage, NUM_ARGS_LOAD_IMAGE,
                          FALSE,
                          app_image,
                          block_io_path,
                          (void *)(kernel_buf),
                          head_buf->kernel_size,
                          &kernel_image);
  if (EFI_ERROR(err)) {
    bub_warning("Could not LOAD kernel_image.\n");
    return 1;
  }

#ifdef BUB_ENABLE_DEBUG
  Print(L"LOADED Kernel Image.\n");
#endif

  // Load parameters
  err = LoadParameters(kernel_image, head_buf, &loaded_kernel_image);
  if (EFI_ERROR(err))
    return 1;

  err = uefi_call_wrapper(BS->StartImage, 3, kernel_image, NULL, NULL);
  if (EFI_ERROR(err)) {
    bub_warning("Could not START kernel_image.\n");
    return 1;
  }

  return 0;
}