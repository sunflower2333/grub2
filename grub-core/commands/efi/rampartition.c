/* rampartition.c - Display/apply Qualcomm EFI RamPartition data. */
/*
 *  GRUB  --  GRand Unified Bootloader
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include <grub/command.h>
#include <grub/dl.h>
#include <grub/err.h>
#include <grub/fdt.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/efi/efi.h>
#include <grub/efi/fdtload.h>
#include <grub/efi/rampartition.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define RAMPARTITION_FDT_EXTRA_SPACE 0x2000
#define QCOM_FDTMEM_MAX_DISABLED_RESERVED 16

static grub_guid_t rampartition_guid = GRUB_EFI_RAMPARTITION_PROTOCOL_GUID;

enum qcom_fdtmem_source
{
  QCOM_FDTMEM_SOURCE_RAMPARTITION,
  QCOM_FDTMEM_SOURCE_FIRMWARE_FDT,
};

static enum qcom_fdtmem_source qcom_fdtmem_source =
  QCOM_FDTMEM_SOURCE_RAMPARTITION;
static char *disabled_reserved_paths[QCOM_FDTMEM_MAX_DISABLED_RESERVED];
static grub_uint32_t disabled_reserved_count;

static grub_efi_rampartition_protocol_t *
grub_efi_locate_rampartition (void)
{
  return grub_efi_locate_protocol (&rampartition_guid, 0);
}

static grub_err_t
read_ram_partitions (grub_efi_rampartition_entry_t **entries,
		     grub_uint32_t *num_entries)
{
  grub_efi_rampartition_protocol_t *proto;
  grub_efi_status_t status;
  grub_uint32_t count = 0;

  *entries = 0;
  *num_entries = 0;

  proto = grub_efi_locate_rampartition ();
  if (!proto)
    return grub_error (GRUB_ERR_BAD_DEVICE,
		       N_("EFI RamPartition protocol not found"));

  status = proto->get_ram_partitions (proto, 0, &count);
  if (status != GRUB_EFI_BUFFER_TOO_SMALL || count == 0)
    return grub_error (GRUB_ERR_BAD_FIRMWARE,
		       N_("failed to query EFI RamPartition count"));

  *entries = grub_zalloc (count * sizeof (**entries));
  if (!*entries)
    return grub_errno;

  status = proto->get_ram_partitions (proto, *entries, &count);
  if (status != GRUB_EFI_SUCCESS || count == 0)
    {
      grub_free (*entries);
      *entries = 0;
      return grub_error (GRUB_ERR_BAD_FIRMWARE,
			 N_("failed to read EFI RamPartition table"));
    }

  *num_entries = count;
  return GRUB_ERR_NONE;
}

static void
print_optional_u32 (const char *name,
		    grub_efi_status_t (__grub_efi_api *fn)
		      (grub_efi_rampartition_protocol_t *, grub_uint32_t *),
		    grub_efi_rampartition_protocol_t *proto)
{
  grub_uint32_t value = 0;

  if (!fn)
    return;

  if (fn (proto, &value) == GRUB_EFI_SUCCESS)
    grub_printf ("%s: %" PRIuGRUB_UINT32_T "\n", name, value);
}

static grub_err_t
grub_cmd_lsefirampart (grub_command_t cmd __attribute__ ((unused)),
		       int argc __attribute__ ((unused)),
		       char **args __attribute__ ((unused)))
{
  grub_efi_rampartition_protocol_t *proto;
  grub_efi_rampartition_entry_t *entries;
  grub_uint32_t count;
  grub_uint32_t major = 0;
  grub_uint32_t minor = 0;
  grub_uint32_t i;
  grub_err_t err;

  proto = grub_efi_locate_rampartition ();
  if (!proto)
    return grub_error (GRUB_ERR_BAD_DEVICE,
		       N_("EFI RamPartition protocol not found"));

  grub_printf ("RamPartition protocol revision: 0x%016" PRIxGRUB_UINT64_T "\n",
	       proto->revision);

  if (proto->get_version
      && proto->get_version (proto, &major, &minor) == GRUB_EFI_SUCCESS)
    grub_printf ("RamPartition table version: %" PRIuGRUB_UINT32_T ".%"
		 PRIuGRUB_UINT32_T "\n", major, minor);

  print_optional_u32 ("Highest bank bit", proto->get_highest_bank_bit, proto);
  print_optional_u32 ("Min PASR size", proto->get_min_pasr_size, proto);

  err = read_ram_partitions (&entries, &count);
  if (err != GRUB_ERR_NONE)
    return err;

  grub_printf ("RAM partitions: %" PRIuGRUB_UINT32_T "\n", count);
  for (i = 0; i < count; i++)
    {
      grub_uint64_t base = entries[i].base;
      grub_uint64_t size = entries[i].available_length;

      grub_printf ("  %02" PRIuGRUB_UINT32_T
		   ": %016" PRIxGRUB_UINT64_T "-%016" PRIxGRUB_UINT64_T
		   " size=%016" PRIxGRUB_UINT64_T "\n",
		   i, base, base + size - 1, size);
    }

  grub_free (entries);
  return GRUB_ERR_NONE;
}

static int
find_memory_node (void *fdt)
{
  int node;
  unsigned int i;
  const char *names[] = {
    "memory",
    "memory@80000000",
    "memory@a0000000"
  };

  for (i = 0; i < ARRAY_SIZE (names); i++)
    {
      node = grub_fdt_find_subnode (fdt, 0, names[i]);
      if (node >= 0)
	return node;
    }

  for (node = grub_fdt_first_node (fdt, 0);
       node >= 0;
       node = grub_fdt_next_node (fdt, node))
    {
      const char *name = grub_fdt_get_nodename (fdt, node);
      grub_uint32_t len = 0;
      const char *device_type;

      if (name && grub_strncmp (name, "memory", sizeof ("memory") - 1) == 0)
	return node;

      device_type = grub_fdt_get_prop (fdt, node, "device_type", &len);
      if (device_type && len >= sizeof ("memory")
	  && grub_memcmp (device_type, "memory", sizeof ("memory")) == 0)
	return node;
    }

  return -1;
}

static int
find_node_by_path (void *fdt, const char *path)
{
  int node = 0;
  const char *p = path;

  if (!path || path[0] != '/')
    return -1;

  while (*p == '/')
    p++;

  while (*p)
    {
      char component[128];
      grub_size_t len = 0;

      while (p[len] && p[len] != '/')
	len++;

      if (len == 0 || len >= sizeof (component))
	return -1;

      grub_memcpy (component, p, len);
      component[len] = '\0';

      node = grub_fdt_find_subnode (fdt, node, component);
      if (node < 0)
	return -1;

      p += len;
      while (*p == '/')
	p++;
    }

  return node;
}

static grub_err_t
set_chosen_u32 (void *fdt, const char *name, grub_uint32_t value)
{
  int node;

  node = grub_fdt_find_subnode (fdt, 0, "chosen");
  if (node < 0)
    node = grub_fdt_add_subnode (fdt, 0, "chosen");
  if (node < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("failed to create /chosen"));

  if (grub_fdt_set_prop32 (fdt, node, name, value) < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("failed to update /chosen"));

  return GRUB_ERR_NONE;
}

static grub_err_t
set_chosen_string (void *fdt, const char *name, const char *value)
{
  int node;

  node = grub_fdt_find_subnode (fdt, 0, "chosen");
  if (node < 0)
    node = grub_fdt_add_subnode (fdt, 0, "chosen");
  if (node < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("failed to create /chosen"));

  if (grub_fdt_set_prop (fdt, node, name, value, grub_strlen (value) + 1) < 0)
    return grub_error (GRUB_ERR_BAD_OS, N_("failed to update /chosen"));

  return GRUB_ERR_NONE;
}

static grub_err_t
apply_ram_partitions_to_loaded_fdt (void *fdt,
				    grub_efi_rampartition_entry_t *entries,
				    grub_uint32_t count)
{
  grub_uint64_t *reg;
  grub_uint32_t i;
  grub_err_t err = GRUB_ERR_NONE;
  int memory_node;
  const char *memory_name = "memory";

  reg = grub_malloc (count * 2 * sizeof (*reg));
  if (!reg)
    return grub_errno;

  for (i = 0; i < count; i++)
    {
      reg[i * 2] = grub_cpu_to_be64 (entries[i].base);
      reg[i * 2 + 1] = grub_cpu_to_be64 (entries[i].available_length);
    }

  if (!fdt)
    {
      err = grub_error (GRUB_ERR_BAD_ARGUMENT, N_("missing FDT"));
      goto out;
    }

  if (grub_fdt_set_prop32 (fdt, 0, "#address-cells", 2) < 0
      || grub_fdt_set_prop32 (fdt, 0, "#size-cells", 2) < 0)
    {
      err = grub_error (GRUB_ERR_BAD_OS,
			N_("failed to update root FDT cell sizes"));
      goto out;
    }

  memory_node = find_memory_node (fdt);
  if (memory_node < 0)
    {
      if (count > 0)
	{
	  static char generated_name[32];

	  grub_snprintf (generated_name, sizeof (generated_name),
			 "memory@%" PRIxGRUB_UINT64_T, entries[0].base);
	  memory_name = generated_name;
	}
      memory_node = grub_fdt_add_subnode (fdt, 0, memory_name);
    }

  if (memory_node < 0)
    {
      err = grub_error (GRUB_ERR_BAD_OS, N_("failed to create /memory node"));
      goto out;
    }

  if (grub_fdt_set_prop (fdt, memory_node, "device_type",
			 "memory", sizeof ("memory")) < 0
      || grub_fdt_set_prop (fdt, memory_node, "reg", reg,
			    count * 2 * sizeof (*reg)) < 0)
    {
      err = grub_error (GRUB_ERR_BAD_OS, N_("failed to update /memory/reg"));
      goto out;
    }

  err = set_chosen_u32 (fdt, "grub,ram-partition-count", count);
  if (err != GRUB_ERR_NONE)
    goto out;

  err = set_chosen_u32 (fdt, "grub,qcom-fdtmem-entry-count", count);
  if (err != GRUB_ERR_NONE)
    goto out;

  err = set_chosen_string (fdt, "grub,qcom-fdtmem-source",
			   "efi-rampartition");
  if (err != GRUB_ERR_NONE)
    goto out;

out:
  grub_free (reg);
  return err;
}

static grub_err_t
apply_firmware_fdt_memory_to_loaded_fdt (void *fdt)
{
  void *firmware_fdt;
  const void *reg;
  grub_uint32_t reg_len = 0;
  grub_uint32_t count;
  int source_node;
  int memory_node;
  grub_err_t err;

  if (!fdt)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("missing FDT"));

  firmware_fdt = grub_efi_get_firmware_fdt ();
  if (!firmware_fdt
      || grub_fdt_check_header_nosize (firmware_fdt) < 0)
    return grub_error (GRUB_ERR_BAD_OS,
		       N_("missing valid firmware FDT"));

  source_node = find_memory_node (firmware_fdt);
  if (source_node < 0)
    return grub_error (GRUB_ERR_BAD_OS,
		       N_("firmware FDT has no memory node"));

  reg = grub_fdt_get_prop (firmware_fdt, source_node, "reg", &reg_len);
  if (!reg || reg_len == 0 || (reg_len % (2 * sizeof (grub_uint64_t))) != 0)
    return grub_error (GRUB_ERR_BAD_OS,
		       N_("firmware FDT has invalid /memory/reg"));

  if (grub_fdt_set_prop32 (fdt, 0, "#address-cells", 2) < 0
      || grub_fdt_set_prop32 (fdt, 0, "#size-cells", 2) < 0)
    return grub_error (GRUB_ERR_BAD_OS,
		       N_("failed to update root FDT cell sizes"));

  memory_node = find_memory_node (fdt);
  if (memory_node < 0)
    {
      memory_node = grub_fdt_add_subnode (fdt, 0, "memory");
      if (memory_node < 0)
	return grub_error (GRUB_ERR_BAD_OS,
			   N_("failed to create /memory node"));
    }

  if (grub_fdt_set_prop (fdt, memory_node, "device_type",
			 "memory", sizeof ("memory")) < 0
      || grub_fdt_set_prop (fdt, memory_node, "reg", reg, reg_len) < 0)
    return grub_error (GRUB_ERR_BAD_OS,
		       N_("failed to copy firmware /memory/reg"));

  count = reg_len / (2 * sizeof (grub_uint64_t));
  err = set_chosen_u32 (fdt, "grub,qcom-fdtmem-entry-count", count);
  if (err != GRUB_ERR_NONE)
    return err;

  return set_chosen_string (fdt, "grub,qcom-fdtmem-source",
			    "firmware-fdt");
}

static grub_err_t
disable_configured_reserved_nodes (void *fdt)
{
  grub_uint32_t disabled = 0;
  grub_uint32_t i;

  for (i = 0; i < disabled_reserved_count; i++)
    {
      const char *path = disabled_reserved_paths[i];
      int node;

      if (grub_strncmp (path, "/reserved-memory/",
			sizeof ("/reserved-memory/") - 1) != 0)
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   N_("reserved-memory path expected: %s"), path);

      node = find_node_by_path (fdt, path);
      if (node < 0)
	continue;

      if (grub_fdt_set_prop (fdt, node, "status", "disabled",
			     sizeof ("disabled")) < 0)
	return grub_error (GRUB_ERR_BAD_OS,
			   N_("failed to disable reserved-memory node: %s"),
			   path);
      disabled++;
    }

  return set_chosen_u32 (fdt, "grub,qcom-disabled-reserved-count", disabled);
}

grub_err_t
EXPORT_FUNC (rampartition_apply_to_fdt) (void *fdt)
{
  grub_efi_rampartition_entry_t *entries;
  grub_uint32_t count;
  grub_err_t err;

  if (qcom_fdtmem_source == QCOM_FDTMEM_SOURCE_FIRMWARE_FDT)
    err = apply_firmware_fdt_memory_to_loaded_fdt (fdt);
  else
    {
      err = read_ram_partitions (&entries, &count);
      if (err != GRUB_ERR_NONE)
	return err;

      err = apply_ram_partitions_to_loaded_fdt (fdt, entries, count);
      grub_free (entries);
    }

  if (err != GRUB_ERR_NONE)
    return err;

  return disable_configured_reserved_nodes (fdt);
}

static void
clear_disabled_reserved_paths (void)
{
  grub_uint32_t i;

  for (i = 0; i < disabled_reserved_count; i++)
    {
      grub_free (disabled_reserved_paths[i]);
      disabled_reserved_paths[i] = 0;
    }
  disabled_reserved_count = 0;
}

static const char *
qcom_fdtmem_source_name (void)
{
  if (qcom_fdtmem_source == QCOM_FDTMEM_SOURCE_FIRMWARE_FDT)
    return "firmware-fdt";
  return "rampartition";
}

static grub_err_t
qcom_fdtmem_set_source (const char *source)
{
  if (grub_strcmp (source, "rampartition") == 0)
    qcom_fdtmem_source = QCOM_FDTMEM_SOURCE_RAMPARTITION;
  else if (grub_strcmp (source, "firmware-fdt") == 0)
    qcom_fdtmem_source = QCOM_FDTMEM_SOURCE_FIRMWARE_FDT;
  else
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_("unknown qcomfdtmem source: %s"), source);

  grub_printf ("qcomfdtmem source: %s\n", qcom_fdtmem_source_name ());
  return GRUB_ERR_NONE;
}

static grub_err_t
qcom_fdtmem_set_disabled_reserved (int argc, char **args)
{
  int i;

  if (argc > QCOM_FDTMEM_MAX_DISABLED_RESERVED)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_("too many reserved-memory paths"));

  clear_disabled_reserved_paths ();

  for (i = 0; i < argc; i++)
    {
      if (grub_strncmp (args[i], "/reserved-memory/",
			sizeof ("/reserved-memory/") - 1) != 0)
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   N_("reserved-memory path expected: %s"), args[i]);

      disabled_reserved_paths[disabled_reserved_count] =
	grub_strdup (args[i]);
      if (!disabled_reserved_paths[disabled_reserved_count])
	return grub_errno;
      disabled_reserved_count++;
    }

  grub_printf ("qcomfdtmem disabled reserved-memory paths: %"
	       PRIuGRUB_UINT32_T "\n", disabled_reserved_count);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_qcomfdtmem (grub_command_t cmd __attribute__ ((unused)),
		     int argc,
		     char **args)
{
  grub_uint32_t i;

  if (argc == 0 || grub_strcmp (args[0], "status") == 0)
    {
      grub_printf ("qcomfdtmem source: %s\n", qcom_fdtmem_source_name ());
      grub_printf ("qcomfdtmem disabled reserved-memory paths: %"
		   PRIuGRUB_UINT32_T "\n", disabled_reserved_count);
      for (i = 0; i < disabled_reserved_count; i++)
	grub_printf ("  %s\n", disabled_reserved_paths[i]);
      return GRUB_ERR_NONE;
    }

  if (grub_strcmp (args[0], "source") == 0)
    {
      if (argc != 2)
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   N_("usage: qcomfdtmem source rampartition|firmware-fdt"));
      return qcom_fdtmem_set_source (args[1]);
    }

  if (grub_strcmp (args[0], "disable-reserved") == 0)
    {
      if (argc < 2)
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   N_("usage: qcomfdtmem disable-reserved <path>..."));
      return qcom_fdtmem_set_disabled_reserved (argc - 1, args + 1);
    }

  if (grub_strcmp (args[0], "clear-reserved") == 0)
    {
      clear_disabled_reserved_paths ();
      return GRUB_ERR_NONE;
    }

  return grub_error (GRUB_ERR_BAD_ARGUMENT,
		     N_("usage: qcomfdtmem source|disable-reserved|clear-reserved|status"));
}

static grub_err_t
grub_cmd_rampartfdt (grub_command_t cmd __attribute__ ((unused)),
		     int argc __attribute__ ((unused)),
		     char **args __attribute__ ((unused)))
{
  grub_efi_rampartition_entry_t *entries;
  grub_uint32_t count;
  grub_err_t err;

  err = read_ram_partitions (&entries, &count);
  if (err != GRUB_ERR_NONE)
    return err;

  {
    void *fdt = grub_fdt_load (RAMPARTITION_FDT_EXTRA_SPACE);

    if (!fdt)
      err = grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("failed to load FDT"));
    else
      err = apply_ram_partitions_to_loaded_fdt (fdt, entries, count);
  }

  if (err == GRUB_ERR_NONE)
    {
      if (grub_fdt_install () != GRUB_ERR_NONE)
	err = grub_error (GRUB_ERR_BAD_OS, N_("failed to install FDT"));
      else
	grub_printf ("Installed %" PRIuGRUB_UINT32_T
		     " EFI RamPartition entries into FDT /memory/reg\n", count);
    }

  grub_free (entries);
  return err;
}

static grub_command_t cmd_lsefirampart;
static grub_command_t cmd_rampartfdt;
static grub_command_t cmd_qcomfdtmem;

GRUB_MOD_INIT(rampartition)
{
  cmd_lsefirampart = grub_register_command ("lsefirampart",
					    grub_cmd_lsefirampart, 0,
					    N_("Display EFI RamPartition table."));
  cmd_rampartfdt = grub_register_command ("rampartfdt",
					  grub_cmd_rampartfdt, 0,
					  N_("Install EFI RamPartition table into FDT."));
  cmd_qcomfdtmem = grub_register_command ("qcomfdtmem",
					  grub_cmd_qcomfdtmem, 0,
					  N_("Configure Qualcomm HLOS FDT memory update."));
}

GRUB_MOD_FINI(rampartition)
{
  grub_unregister_command (cmd_lsefirampart);
  grub_unregister_command (cmd_rampartfdt);
  grub_unregister_command (cmd_qcomfdtmem);
  clear_disabled_reserved_paths ();
}
