/*
 * linuxdirect.c - Direct ARM64 Linux Image loader for EFI GRUB.
 *
 * This loader intentionally bypasses the Linux EFI stub. It is meant for
 * Qualcomm platforms where firmware exposes a better RamPartition table than
 * the EFI memory map handed to the stub.
 */

#include <grub/cache.h>
#include <grub/command.h>
#include <grub/dl.h>
#include <grub/err.h>
#include <grub/fdt.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/lib/cmdline.h>
#include <grub/linux.h>
#include <grub/loader.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/verify.h>
#include <grub/efi/efi.h>
#include <grub/efi/fdtload.h>
#include <grub/efi/memory.h>
#include <grub/efi/rampartition.h>
#include <grub/cpu/efi/memory.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define ARM64_IMAGE_MAGIC 0x644d5241
#define ARM64_DEFAULT_TEXT_OFFSET 0x80000
#define ARM64_KERNEL_BASE_ALIGN 0x200000
#define ARM64_FDT_EXTRA_SPACE 0x4000
#define ARM64_INITRD_MAX_ADDRESS_OFFSET (32ULL * 1024 * 1024 * 1024)

struct arm64_image_header
{
  grub_uint32_t code0;
  grub_uint32_t code1;
  grub_uint64_t text_offset;
  grub_uint64_t image_size;
  grub_uint64_t flags;
  grub_uint64_t res2;
  grub_uint64_t res3;
  grub_uint64_t res4;
  grub_uint32_t magic;
  grub_uint32_t res5;
};

extern void grub_arm64_linux_jump (grub_addr_t fdt,
				   grub_addr_t kernel_entry) __attribute__ ((noreturn));

static grub_dl_t my_mod;
static grub_command_t cmd_linuxdirect;
static grub_command_t cmd_initrddirect;

static int loaded;
static grub_addr_t kernel_alloc_addr;
static grub_addr_t kernel_addr;
static grub_size_t kernel_alloc_size;
static grub_size_t kernel_file_size;
static grub_uint64_t kernel_image_size;
static char *linux_args;
static grub_addr_t initrd_start;
static grub_addr_t initrd_end;
static struct grub_linux_initrd_context initrd_ctx = { 0, 0, 0 };

static grub_uint64_t
align_up_u64 (grub_uint64_t value, grub_uint64_t align)
{
  return (value + align - 1) & ~(align - 1);
}

static grub_err_t
prepare_fdt (void **fdt_out)
{
  void *fdt;
  int node;
  grub_uint32_t compatible_len = 0;

  fdt = grub_fdt_load (ARM64_FDT_EXTRA_SPACE + grub_strlen (linux_args));
  if (!fdt)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("failed to load FDT"));

  if (!grub_fdt_get_prop (fdt, 0, "compatible", &compatible_len)
      || compatible_len == 0
      || grub_fdt_find_subnode (fdt, 0, "cpus") < 0)
    return grub_error (GRUB_ERR_BAD_OS,
		       N_("missing machine FDT; load devicetree or install firmware FDT first"));

  if (rampartition_apply_to_fdt (fdt) != GRUB_ERR_NONE)
    return grub_errno;

  node = grub_fdt_find_subnode (fdt, 0, "chosen");
  if (node < 0)
    node = grub_fdt_add_subnode (fdt, 0, "chosen");
  if (node < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("failed to create /chosen"));

  if (grub_fdt_set_prop (fdt, node, "bootargs", linux_args,
			 grub_strlen (linux_args) + 1) < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("failed to set bootargs"));

  if (initrd_start && initrd_end > initrd_start)
    {
      if (grub_fdt_set_prop64 (fdt, node, "linux,initrd-start",
			       initrd_start) < 0
	  || grub_fdt_set_prop64 (fdt, node, "linux,initrd-end",
				  initrd_end) < 0)
	return grub_error (GRUB_ERR_BAD_OS, N_("failed to set initrd"));
    }

  if (grub_fdt_check_header_nosize (fdt) < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("invalid final FDT"));

  *fdt_out = fdt;
  return GRUB_ERR_NONE;
}

static grub_err_t
linuxdirect_boot (void)
{
  void *fdt = NULL;
  grub_err_t err;

  err = prepare_fdt (&fdt);
  if (err != GRUB_ERR_NONE)
    {
      grub_fdt_unload ();
      return err;
    }

  grub_arch_sync_caches ((void *) kernel_alloc_addr, kernel_alloc_size);
  grub_arch_sync_caches (fdt, grub_fdt_get_totalsize (fdt));
  if (initrd_start && initrd_end > initrd_start)
    grub_arch_sync_caches ((void *) initrd_start, initrd_end - initrd_start);

  if (grub_efi_finish_boot_services (NULL, NULL, NULL, NULL, NULL) != GRUB_ERR_NONE)
    {
      grub_fdt_unload ();
      return grub_errno;
    }

  grub_arm64_linux_jump ((grub_addr_t) fdt, kernel_addr);
}

static grub_err_t
linuxdirect_unload (void)
{
  grub_dl_unref (my_mod);
  loaded = 0;

  grub_free (linux_args);
  linux_args = NULL;

  if (kernel_alloc_addr)
    grub_efi_free_pages (kernel_alloc_addr,
			 GRUB_EFI_BYTES_TO_PAGES (kernel_alloc_size));
  kernel_alloc_addr = 0;
  kernel_addr = 0;
  kernel_alloc_size = 0;
  kernel_file_size = 0;
  kernel_image_size = 0;

  if (initrd_start)
    grub_efi_free_pages (initrd_start,
			 GRUB_EFI_BYTES_TO_PAGES (initrd_end - initrd_start));
  initrd_start = initrd_end = 0;
  grub_initrd_close (&initrd_ctx);

  grub_fdt_unload ();
  return GRUB_ERR_NONE;
}

static grub_err_t
load_kernel (grub_file_t file, const char *filename)
{
  struct arm64_image_header header;
  grub_uint64_t text_offset;
  grub_uint64_t image_size;
  grub_uint64_t base;
  grub_uint64_t alloc_size;
  grub_uint64_t alloc_request_size;
  grub_uint64_t alloc_end;
  grub_ssize_t read_size;

  if (grub_file_read (file, &header, sizeof (header)) != sizeof (header))
    return grub_error (GRUB_ERR_FILE_READ_ERROR, N_("failed to read Linux header"));

  if (grub_le_to_cpu32 (header.magic) != ARM64_IMAGE_MAGIC)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not an ARM64 Linux Image"));

  text_offset = grub_le_to_cpu64 (header.image_size) ?
    grub_le_to_cpu64 (header.text_offset) : ARM64_DEFAULT_TEXT_OFFSET;
  image_size = grub_le_to_cpu64 (header.image_size);

  kernel_file_size = grub_file_size (file);
  if (image_size == 0 || image_size < kernel_file_size)
    image_size = kernel_file_size;
  kernel_image_size = image_size;

  alloc_size = align_up_u64 (text_offset + image_size, GRUB_EFI_PAGE_SIZE);
  alloc_request_size = alloc_size + ARM64_KERNEL_BASE_ALIGN;
  kernel_alloc_size = alloc_request_size;
  kernel_alloc_addr = (grub_addr_t) grub_efi_allocate_pages_real (
    GRUB_EFI_MAX_USABLE_ADDRESS, GRUB_EFI_BYTES_TO_PAGES (alloc_request_size),
    GRUB_EFI_ALLOCATE_MAX_ADDRESS, GRUB_EFI_LOADER_DATA);
  if (!kernel_alloc_addr)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("failed to allocate kernel memory"));

  base = align_up_u64 (kernel_alloc_addr, ARM64_KERNEL_BASE_ALIGN);
  kernel_addr = base + text_offset;
  alloc_end = kernel_alloc_addr + alloc_request_size;
  if (kernel_addr + image_size > alloc_end)
    {
      grub_efi_free_pages (kernel_alloc_addr,
			   GRUB_EFI_BYTES_TO_PAGES (kernel_alloc_size));
      kernel_alloc_addr = 0;
      kernel_addr = 0;
      return grub_error (GRUB_ERR_OUT_OF_MEMORY,
			 N_("failed to align kernel memory"));
    }

  grub_memset ((void *) kernel_alloc_addr, 0, kernel_alloc_size);
  grub_file_seek (file, 0);
  read_size = grub_file_read (file, (void *) kernel_addr, kernel_file_size);
  if (read_size < (grub_ssize_t) kernel_file_size)
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"), filename);
      return grub_errno;
    }

  grub_dprintf ("linuxdirect",
		"kernel base=0x%" PRIxGRUB_UINT64_T " entry=0x%" PRIxGRUB_ADDR
		" text_offset=0x%" PRIxGRUB_UINT64_T
		" file=0x%" PRIxGRUB_SIZE " image=0x%" PRIxGRUB_UINT64_T "\n",
		base, kernel_addr, text_offset, kernel_file_size, kernel_image_size);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_linuxdirect (grub_command_t cmd __attribute__ ((unused)),
		      int argc, char **argv)
{
  grub_file_t file = NULL;
  grub_err_t err;
  int size;

  grub_dl_ref (my_mod);

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
      goto fail;
    }

  grub_loader_unset ();

  file = grub_file_open (argv[0], GRUB_FILE_TYPE_LINUX_KERNEL);
  if (!file)
    goto fail;

  err = load_kernel (file, argv[0]);
  grub_file_close (file);
  file = NULL;
  if (err != GRUB_ERR_NONE)
    goto fail;

  size = grub_loader_cmdline_size (argc, argv) + sizeof (LINUX_IMAGE);
  linux_args = grub_malloc (size);
  if (!linux_args)
    goto fail;

  grub_memcpy (linux_args, LINUX_IMAGE, sizeof (LINUX_IMAGE));
  err = grub_create_loader_cmdline (argc, argv,
				    linux_args + sizeof (LINUX_IMAGE) - 1,
				    size, GRUB_VERIFY_KERNEL_CMDLINE);
  if (err != GRUB_ERR_NONE)
    goto fail;

  loaded = 1;
  grub_loader_set (linuxdirect_boot, linuxdirect_unload,
		   GRUB_LOADER_FLAG_NORETURN);
  return GRUB_ERR_NONE;

fail:
  if (file)
    grub_file_close (file);
  if (kernel_alloc_addr)
    {
      grub_efi_free_pages (kernel_alloc_addr,
			   GRUB_EFI_BYTES_TO_PAGES (kernel_alloc_size));
      kernel_alloc_addr = 0;
      kernel_addr = 0;
    }
  grub_free (linux_args);
  linux_args = NULL;
  loaded = 0;
  grub_dl_unref (my_mod);
  return grub_errno;
}

static void *
allocate_initrd_mem (grub_size_t initrd_size)
{
  grub_addr_t max_addr;

  if (grub_efi_get_ram_base (&max_addr) != GRUB_ERR_NONE)
    return NULL;

  max_addr += ARM64_INITRD_MAX_ADDRESS_OFFSET - 1;
  return grub_efi_allocate_pages_real (max_addr,
				       GRUB_EFI_BYTES_TO_PAGES (initrd_size),
				       GRUB_EFI_ALLOCATE_MAX_ADDRESS,
				       GRUB_EFI_LOADER_DATA);
}

static grub_err_t
grub_cmd_initrddirect (grub_command_t cmd __attribute__ ((unused)),
		       int argc, char **argv)
{
  grub_size_t initrd_size;
  void *initrd_mem;

  if (!loaded)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("you need to load the kernel first"));

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  if (grub_initrd_init (argc, argv, &initrd_ctx))
    goto fail;

  initrd_size = grub_get_initrd_size (&initrd_ctx);
  initrd_mem = allocate_initrd_mem (initrd_size);
  if (!initrd_mem)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto fail;
    }

  if (grub_initrd_load (&initrd_ctx, initrd_mem))
    {
      grub_efi_free_pages ((grub_addr_t) initrd_mem,
			   GRUB_EFI_BYTES_TO_PAGES (initrd_size));
      goto fail;
    }

  initrd_start = (grub_addr_t) initrd_mem;
  initrd_end = initrd_start + initrd_size;
  grub_initrd_close (&initrd_ctx);
  return GRUB_ERR_NONE;

fail:
  grub_initrd_close (&initrd_ctx);
  return grub_errno;
}

GRUB_MOD_INIT (linuxdirect)
{
  cmd_linuxdirect = grub_register_command ("linuxdirect",
					   grub_cmd_linuxdirect, 0,
					   N_("Load ARM64 Linux Image directly."));
  cmd_initrddirect = grub_register_command ("initrddirect",
					    grub_cmd_initrddirect, 0,
					    N_("Load initrd for linuxdirect."));
  my_mod = mod;
}

GRUB_MOD_FINI (linuxdirect)
{
  grub_unregister_command (cmd_linuxdirect);
  grub_unregister_command (cmd_initrddirect);
}
