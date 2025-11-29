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
 * GRUB_EFI_GLOBAL_VARIABLE_GUID. Optional --set <env> stores value
 * into a GRUB variable instead of printing. Format options:
 * --string: UTF-16LE to UTF-8 conversion
 * --uint8/--uint16/--uint32/--uint64: binary integer parsing (little-endian)
 */

typedef struct {
  grub_uint32_t d1;
  grub_uint16_t d2;
  grub_uint16_t d3;
  grub_uint8_t  d4[8];
} simple_guid_t;

typedef enum {
  FORMAT_STRING,
  FORMAT_UINT8,
  FORMAT_UINT16,
  FORMAT_UINT32,
  FORMAT_UINT64
} output_format_t;

static char *
utf16le_to_utf8 (const grub_uint8_t *data, grub_size_t datasize)
{
  /* Simple UTF-16LE to UTF-8 conversion (ASCII subset, ignoring surrogates) */
  grub_size_t out_len = 0;
  grub_size_t i;
  
  /* Calculate output length */
  for (i = 0; i + 1 < datasize; i += 2)
  {
    grub_uint16_t c = data[i] | (data[i + 1] << 8);
    if (c == 0)
      break;
    if (c < 0x80)
      out_len += 1;
    else if (c < 0x800)
      out_len += 2;
    else
      out_len += 3;
  }
  
  char *out = grub_malloc (out_len + 1);
  if (!out)
    return NULL;
  
  grub_size_t pos = 0;
  for (i = 0; i + 1 < datasize; i += 2)
  {
    grub_uint16_t c = data[i] | (data[i + 1] << 8);
    if (c == 0)
      break;
    
    if (c < 0x80)
    {
      out[pos++] = (char)c;
    }
    else if (c < 0x800)
    {
      out[pos++] = (char)(0xc0 | (c >> 6));
      out[pos++] = (char)(0x80 | (c & 0x3f));
    }
    else
    {
      out[pos++] = (char)(0xe0 | (c >> 12));
      out[pos++] = (char)(0x80 | ((c >> 6) & 0x3f));
      out[pos++] = (char)(0x80 | (c & 0x3f));
    }
  }
  out[pos] = '\0';
  return out;
}

static char *
format_as_uint (const grub_uint8_t *data, grub_size_t datasize, output_format_t format)
{
  grub_uint64_t value = 0;
  grub_size_t required_size = 0;
  char *out;
  
  switch (format)
  {
    case FORMAT_UINT8:
      required_size = 1;
      break;
    case FORMAT_UINT16:
      required_size = 2;
      break;
    case FORMAT_UINT32:
      required_size = 4;
      break;
    case FORMAT_UINT64:
      required_size = 8;
      break;
    default:
      return NULL;
  }
  
  if (datasize < required_size)
    return NULL;
  
  /* Parse little-endian */
  for (grub_size_t i = 0; i < required_size; i++)
    value |= ((grub_uint64_t)data[i]) << (i * 8);
  
  out = grub_malloc (32);
  if (!out)
    return NULL;
  
  grub_snprintf (out, 32, "%llu", (unsigned long long)value);
  return out;
}

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
  output_format_t format = FORMAT_STRING;
  grub_guid_t guid_local;
  const grub_guid_t *guid = &global;
  grub_size_t datasize = 0;
  void *data = NULL;
  grub_err_t err;
  int arg_idx = 0;

  /* Syntax:
   * efivar [--string|--uint8|--uint16|--uint32|--uint64] <name> [<guid>]
   * efivar --set <env> [--string|--uint8|--uint16|--uint32|--uint64] <name> [<guid>]
   */
  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Usage: efivar [--string|--uintN] <name> [<guid>] | efivar --set <env> [--string|--uintN] <name> [<guid>]"));

  /* Parse --set option */
  if (grub_strcmp (args[arg_idx], "--set") == 0)
  {
    if (argc < 3)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Usage: efivar --set <env> [--string|--uintN] <name> [<guid>]"));
    arg_idx++;
    set_env = args[arg_idx++];
  }

  /* Parse format option */
  if (arg_idx < argc)
  {
    if (grub_strcmp (args[arg_idx], "--string") == 0)
    {
      format = FORMAT_STRING;
      arg_idx++;
    }
    else if (grub_strcmp (args[arg_idx], "--uint8") == 0)
    {
      format = FORMAT_UINT8;
      arg_idx++;
    }
    else if (grub_strcmp (args[arg_idx], "--uint16") == 0)
    {
      format = FORMAT_UINT16;
      arg_idx++;
    }
    else if (grub_strcmp (args[arg_idx], "--uint32") == 0)
    {
      format = FORMAT_UINT32;
      arg_idx++;
    }
    else if (grub_strcmp (args[arg_idx], "--uint64") == 0)
    {
      format = FORMAT_UINT64;
      arg_idx++;
    }
  }

  /* Parse name */
  if (arg_idx >= argc)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Missing variable name"));
  name = args[arg_idx++];

  /* Parse optional GUID */
  if (arg_idx < argc)
    guid_str = args[arg_idx];

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

  /* Format output based on selected format */
  const grub_uint8_t *p = (const grub_uint8_t *) data;
  char *output = NULL;

  if (format == FORMAT_STRING)
  {
    output = utf16le_to_utf8 (p, datasize);
  }
  else
  {
    output = format_as_uint (p, datasize, format);
  }

  if (!output)
  {
    grub_free (data);
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("failed to format output"));
  }

  if (set_env)
  {
    /* Store into GRUB environment variable */
    grub_env_set (set_env, output);
  }
  else
  {
    grub_printf ("%s\n", output);
  }

  grub_free (data);
  grub_free (output);
  return GRUB_ERR_NONE;
}

static grub_command_t cmd = NULL;

GRUB_MOD_INIT (efivar)
{
  cmd = grub_register_command ("efivar", grub_cmd_efivar, NULL,
                               N_("Read an EFI variable. Usage: efivar [--string|--uint8|--uint16|--uint32|--uint64] <name> [<guid>] | efivar --set <env> [format] <name> [<guid>]"));
}

GRUB_MOD_FINI (efivar)
{
  if (cmd)
    grub_unregister_command (cmd);
}
