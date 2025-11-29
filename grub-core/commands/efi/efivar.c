/* efivar.c - Read an EFI variable via grub_efi_get_variable. */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2025  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/types.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/command.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* Supports specifying GUID via command argument or defaults to
 * GRUB_EFI_GLOBAL_VARIABLE_GUID. Optional --set <env> stores hex string
 * into a GRUB variable instead of printing. */

typedef struct {
  grub_uint32_t d1;
  grub_uint16_t d2;
  grub_uint16_t d3;
  grub_uint8_t  d4[8];
} simple_guid_t;

static int hexval (char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex_bytes (const char *s, grub_size_t nbytes, grub_uint8_t *out)
{
  /* Parse 2*nbytes hex chars (no separators). */
  for (grub_size_t i = 0; i < nbytes; i++)
  {
    int h = hexval (s[i*2]);
    int l = hexval (s[i*2+1]);
    if (h < 0 || l < 0)
      return -1;
    out[i] = (grub_uint8_t)((h << 4) | l);
  }
  return 0;
}

static int parse_guid (const char *s, grub_guid_t *guid)
{
  /* Expect XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
  if (!s)
    return -1;
  const char *p = s;
  /* Check positions of dashes */
  if (grub_strlen (s) != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
    return -1;

  /* d1: 8 hex */
  grub_uint8_t tmp[16];
  if (parse_hex_bytes (p, 4, tmp) < 0) return -1;
  guid->data1 = ((grub_uint32_t)tmp[0] << 24) | ((grub_uint32_t)tmp[1] << 16) | ((grub_uint32_t)tmp[2] << 8) | tmp[3];
  p += 9; /* skip dashes */

  /* d2: 4 hex */
  if (parse_hex_bytes (p, 2, tmp) < 0) return -1;
  guid->data2 = ((grub_uint16_t)tmp[0] << 8) | tmp[1];
  p += 5;

  /* d3: 4 hex */
  if (parse_hex_bytes (p, 2, tmp) < 0) return -1;
  guid->data3 = ((grub_uint16_t)tmp[0] << 8) | tmp[1];
  p += 5;

  /* d4: 4 hex + 12 hex -> total 16 hex = 8 bytes */
  /* First 2 bytes */
  if (parse_hex_bytes (p, 2, guid->data4) < 0) return -1;
  p += 5; /* includes dash */
  if (parse_hex_bytes (p, 6, guid->data4 + 2) < 0) return -1;

  return 0;
}

static grub_err_t
grub_cmd_efivar (grub_command_t cmd __attribute__ ((unused)),
                 int argc, char **args)
{
  static grub_guid_t global = GRUB_EFI_GLOBAL_VARIABLE_GUID;
  const char *name = NULL;
  const char *guid_str = NULL;
  const char *set_env = NULL;
  grub_guid_t guid_local;
  const grub_guid_t *guid = &global;
  grub_size_t datasize = 0;
  void *data = NULL;
  grub_err_t err;

  /* Syntax:
   * efivar <name> [<guid>]
   * efivar --set <env> <name> [<guid>]
   */
  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Usage: efivar <name> [<guid>] | efivar --set <env> <name> [<guid>]"));

  if (grub_strcmp (args[0], "--set") == 0)
  {
    if (argc < 3)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Usage: efivar --set <env> <name> [<guid>]"));
    set_env = args[1];
    name = args[2];
    if (argc >= 4)
      guid_str = args[3];
  }
  else
  {
    name = args[0];
    if (argc >= 2)
      guid_str = args[1];
  }

  if (guid_str)
  {
    if (parse_guid (guid_str, &guid_local) != 0)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid GUID format. Expected XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"));
    guid = &guid_local;
  }

  /* Read variable */
  err = grub_efi_get_variable (name, guid, &datasize, &data);
  if (err != GRUB_ERR_NONE)
    return err;

  if (data == NULL || datasize == 0)
  {
    grub_printf ("\n");
    return GRUB_ERR_NONE;
  }

  /* Prepare hex string */
  const grub_uint8_t *p = (const grub_uint8_t *) data;
  /* Each byte -> 2 chars + space, last without space. */
  grub_size_t outlen = datasize ? (datasize * 3 - 1) : 0;
  char *hex = NULL;
  if (outlen > 0)
  {
    hex = grub_malloc (outlen + 1);
    if (!hex)
    {
      grub_free (data);
      return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
    }
    for (grub_size_t i = 0; i < datasize; i++)
    {
      grub_snprintf (hex + i * 3, 3 + 1, "%02x", (unsigned)p[i]);
      if (i + 1 < datasize)
        hex[i * 3 + 2] = ' ';
    }
    hex[outlen] = '\0';
  }

  if (set_env)
  {
    /* Store into GRUB environment variable */
    grub_env_set (set_env, hex ? hex : "");
  }
  else
  {
    grub_printf ("%s\n", hex ? hex : "");
  }

  grub_free (data);
  grub_free (hex);
  return GRUB_ERR_NONE;
}

static grub_command_t cmd = NULL;

GRUB_MOD_INIT (efivar)
{
  cmd = grub_register_command ("efivar", grub_cmd_efivar, NULL,
                               N_("Read an EFI variable and print/store as hex. Usage: efivar <name> [<guid>] | efivar --set <env> <name> [<guid>]"));
}

GRUB_MOD_FINI (efivar)
{
  if (cmd)
    grub_unregister_command (cmd);
}
