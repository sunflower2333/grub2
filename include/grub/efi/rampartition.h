/* rampartition.h - Qualcomm EFI RamPartition protocol. */
/*
 *  GRUB  --  GRand Unified Bootloader
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef GRUB_EFI_RAMPARTITION_HEADER
#define GRUB_EFI_RAMPARTITION_HEADER 1

#include <grub/err.h>
#include <grub/efi/api.h>
#include <grub/symbol.h>

#define GRUB_EFI_RAMPARTITION_PROTOCOL_GUID \
  { 0x5172ffb5, 0x4253, 0x7d51, \
    { 0xc6, 0x41, 0xa7, 0x01, 0xf9, 0x73, 0x10, 0x3c } \
  }

#define GRUB_EFI_RAMPARTITION_PROTOCOL_REVISION 0x0000000000010001ULL

typedef struct grub_efi_rampartition_protocol grub_efi_rampartition_protocol_t;

struct grub_efi_rampartition_entry
{
  grub_uint64_t base;
  grub_uint64_t available_length;
};
typedef struct grub_efi_rampartition_entry grub_efi_rampartition_entry_t;

typedef grub_efi_status_t
(__grub_efi_api *grub_efi_rampartition_get_version_t)
  (grub_efi_rampartition_protocol_t *this,
   grub_uint32_t *major_version,
   grub_uint32_t *minor_version);

typedef grub_efi_status_t
(__grub_efi_api *grub_efi_rampartition_get_highest_bank_bit_t)
  (grub_efi_rampartition_protocol_t *this,
   grub_uint32_t *highest_bank_bit);

typedef grub_efi_status_t
(__grub_efi_api *grub_efi_rampartition_get_ram_partitions_t)
  (grub_efi_rampartition_protocol_t *this,
   grub_efi_rampartition_entry_t *ram_partitions,
   grub_uint32_t *num_partitions);

typedef grub_efi_status_t
(__grub_efi_api *grub_efi_rampartition_get_min_pasr_size_t)
  (grub_efi_rampartition_protocol_t *this,
   grub_uint32_t *min_pasr_size);

struct grub_efi_rampartition_protocol
{
  grub_uint64_t revision;
  grub_efi_rampartition_get_version_t get_version;
  grub_efi_rampartition_get_highest_bank_bit_t get_highest_bank_bit;
  grub_efi_rampartition_get_ram_partitions_t get_ram_partitions;
  grub_efi_rampartition_get_min_pasr_size_t get_min_pasr_size;
};

grub_err_t EXPORT_FUNC (rampartition_apply_to_fdt) (void *fdt);

#endif
