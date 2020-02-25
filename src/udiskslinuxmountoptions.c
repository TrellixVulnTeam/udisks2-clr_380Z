/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 * Copyright (C) 2020 Tomas Bzatek <tbzatek@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>

#include <libmount/libmount.h>

#include "udiskslogging.h"
#include "udiskslinuxmountoptions.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udisksstate.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxdevice.h"
#include "udisksconfigmanager.h"
#include "udisks-daemon-resources.h"


/* ---------------------------------------------------------------------------------------------------- */

static GHashTable * mount_options_parse_config_file (const gchar *filename, GError **error);
static GHashTable * mount_options_get_from_udev (UDisksLinuxDevice *device, GError **error);

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  gchar **defaults;
  gchar **allow;
  gchar **allow_uid_self;
  gchar **allow_gid_self;
} FSMountOptions;

static void
free_fs_mount_options (FSMountOptions *options)
{
  if (options)
    {
      g_strfreev (options->defaults);
      g_strfreev (options->allow);
      g_strfreev (options->allow_uid_self);
      g_strfreev (options->allow_gid_self);
      g_free (options);
    }
}

static void
strv_append_unique (gchar **src, gchar ***dest)
{
  guint src_len;
  guint dest_len;
  gchar **s;
  gchar **l;
  guint l_len = 0;

  g_warn_if_fail (dest != NULL);

  if (!src || g_strv_length (src) == 0)
    return;

  if (!*dest)
    {
      *dest = g_strdupv (src);
      return;
    }

  src_len = g_strv_length (src);
  dest_len = g_strv_length (*dest);

  l = g_malloc (src_len * sizeof (gpointer));
  for (s = src; *s; s++)
    if (!g_strv_contains ((const gchar * const *) *dest, *s))
      l[l_len++] = g_strdup (*s);

  if (l_len > 0)
    {
      *dest = g_realloc (*dest, (dest_len + l_len + 1) * sizeof (gpointer));
      memcpy (*dest + dest_len, l, l_len * sizeof (gpointer));
      (*dest)[dest_len + l_len] = NULL;
    }

  g_free (l);
}

static void
append_fs_mount_options (const FSMountOptions *src, FSMountOptions *dest)
{
  if (!src)
    return;

  strv_append_unique (src->defaults, &dest->defaults);
  strv_append_unique (src->allow, &dest->allow);
  strv_append_unique (src->allow_uid_self, &dest->allow_uid_self);
  strv_append_unique (src->allow_gid_self, &dest->allow_gid_self);
}

/* Similar to append_fs_mount_options() but replaces the member data instead of appending */
static void
override_fs_mount_options (const FSMountOptions *src, FSMountOptions *dest)
{
  if (!src)
    return;

#define OVERRIDE_MEMBER(m) \
  if (src->m) \
    { \
      g_strfreev (dest->m); \
      dest->m = g_strdupv (src->m); \
    }

  OVERRIDE_MEMBER (defaults);
  OVERRIDE_MEMBER (allow);
  OVERRIDE_MEMBER (allow_uid_self);
  OVERRIDE_MEMBER (allow_gid_self);
}

/* ---------------------------------------------------------------------------------------------------- */

#define MOUNT_OPTIONS_GLOBAL_CONFIG_FILE_NAME "mount_options.conf"

#define MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS  "defaults"
#define MOUNT_OPTIONS_KEY_DEFAULTS           "defaults"
#define MOUNT_OPTIONS_KEY_ALLOW              "allow"
#define MOUNT_OPTIONS_KEY_ALLOW_UID_SELF     "allow_uid_self"
#define MOUNT_OPTIONS_KEY_ALLOW_GID_SELF     "allow_gid_self"
#define UDEV_MOUNT_OPTIONS_PREFIX            "UDISKS_MOUNT_OPTIONS_"

/*
 * compute_block_level_mount_options: <internal>
 * @daemon: A #UDisksDaemon.
 * @block: A #UDisksBlock.
 * @fstype: The filesystem type to match or %NULL.
 * @fsmo: Filesystem type specific #FSMountOptions.
 * @fsmo_any: General ("any") #FSMountOptions.
 *
 * Calculate mount options for the given level of overrides. Matches the block
 * device-specific options on top of the defaults.
 *
 * Returns: %TRUE when mount options were overriden, %FALSE otherwise.
 */
static gboolean
compute_block_level_mount_options (GHashTable      *opts,
                                   UDisksBlock     *block,
                                   const gchar     *fstype,
                                   FSMountOptions  *fsmo,
                                   FSMountOptions  *fsmo_any)
{
  GHashTable *general_options;
  GHashTable *block_options;
  gboolean changed = FALSE;

  /* Compute general defaults first */
  general_options = g_hash_table_lookup (opts, MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS);
  if (general_options)
    {
      FSMountOptions *o;

      o = g_hash_table_lookup (general_options, MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS);
      override_fs_mount_options (o, fsmo_any);
      changed = changed || o != NULL;

      o = fstype ? g_hash_table_lookup (general_options, fstype) : NULL;
      override_fs_mount_options (o, fsmo);
      changed = changed || o != NULL;
    }

  /* Match specific block device */
  block_options = NULL;
  if (block)
    {
      const gchar *block_device;
      const gchar * const *block_symlinks;
      GList *keys;
      GList *l;

      block_device = udisks_block_get_device (block);
      block_symlinks = udisks_block_get_symlinks (block);

      keys = g_hash_table_get_keys (opts);
      g_warn_if_fail (keys != NULL);
      for (l = keys; l != NULL; l = l->next)
        {
          if (!l->data || g_str_equal (l->data, MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS))
            continue;
          if (g_str_equal (l->data, block_device) ||
              (block_symlinks && g_strv_contains (block_symlinks, l->data)))
            {
              block_options = g_hash_table_lookup (opts, l->data);
              break;
            }
        }
      g_list_free (keys);
    }

  /* Block device specific options should fully override "general" options per-member basis */
  if (block_options)
    {
      FSMountOptions *o;

      o = g_hash_table_lookup (block_options, MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS);
      override_fs_mount_options (o, fsmo_any);
      changed = changed || o != NULL;

      o = fstype ? g_hash_table_lookup (block_options, fstype) : NULL;
      override_fs_mount_options (o, fsmo);
      changed = changed || o != NULL;
    }

  return changed;
}

/*
 * compute_mount_options_for_fs_type: <internal>
 * @daemon: A #UDisksDaemon.
 * @block: A #UDisksBlock.
 * @object: A #UDisksLinuxBlockObject.
 * @fstype: The filesystem type to use or %NULL.
 *
 * Calculate mount options across different levels of overrides
 * (builtin, global config, local user config).
 *
 * Returns: (transfer full): Newly allocated #FSMountOptions options. Free with free_fs_mount_options().
 */
static FSMountOptions *
compute_mount_options_for_fs_type (UDisksDaemon           *daemon,
                                   UDisksBlock            *block,
                                   UDisksLinuxBlockObject *object,
                                   const gchar            *fstype)
{
  UDisksConfigManager *config_manager;
  UDisksLinuxDevice *device;
  GHashTable *builtin_opts;
  GHashTable *overrides;
  FSMountOptions *fsmo;
  FSMountOptions *fsmo_any;
  gchar *config_file_path;
  GError *error = NULL;
  gboolean changed = FALSE;

  config_manager = udisks_daemon_get_config_manager (daemon);

  fsmo = g_malloc0 (sizeof (FSMountOptions));
  fsmo_any = g_malloc0 (sizeof (FSMountOptions));

  /* Builtin options, two-level hashtable */
  builtin_opts = g_object_get_data (G_OBJECT (daemon), "mount-options");
  g_return_val_if_fail (builtin_opts != NULL, NULL);
  compute_block_level_mount_options (builtin_opts, block, fstype, fsmo, fsmo_any);

  /* Global config file overrides, two-level hashtable */
  config_file_path = g_build_filename (udisks_config_manager_get_config_dir (config_manager),
                                       MOUNT_OPTIONS_GLOBAL_CONFIG_FILE_NAME, NULL);
  overrides = mount_options_parse_config_file (config_file_path, &error);
  if (overrides)
    {
      changed = compute_block_level_mount_options (overrides, block, fstype, fsmo, fsmo_any);
      g_hash_table_unref (overrides);
    }
  else
    {
      if (! g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT) /* not found */ &&
          ! g_error_matches (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED) /* empty file */ )
        {
          udisks_warning ("Error reading global mount options config file %s: %s",
                          config_file_path, error->message);
        }
      g_clear_error (&error);
    }
  g_free (config_file_path);

  /* udev properties, single-level hashtable */
  device = udisks_linux_block_object_get_device (object);
  overrides = mount_options_get_from_udev (device, &error);
  if (overrides)
    {
      FSMountOptions *o;

      o = g_hash_table_lookup (overrides, MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS);
      override_fs_mount_options (o, fsmo_any);
      changed = changed || o != NULL;

      o = fstype ? g_hash_table_lookup (overrides, fstype) : NULL;
      override_fs_mount_options (o, fsmo);
      changed = changed || o != NULL;

      g_hash_table_unref (overrides);
    }
  else
    {
      udisks_warning ("Error getting udev mount options: %s",
                      error->message);
      g_clear_error (&error);
    }
  g_object_unref (device);

  /* Merge "any" and fstype-specific options */
  append_fs_mount_options (fsmo_any, fsmo);
  free_fs_mount_options (fsmo_any);
  fsmo_any = NULL;

  if (changed && fsmo->defaults)
    {
      gchar *opts = g_strjoinv (",", fsmo->defaults);
      udisks_notice ("Using overriden mount options: %s", opts);
      g_free (opts);
    }

  return fsmo;
}

/* ---------------------------------------------------------------------------------------------------- */

/* transfer-full */
static gchar **
parse_mount_options_string (const gchar *str)
{
  GPtrArray *opts;
  char *optstr;
  char *name;
  size_t namesz;
  char *value;
  size_t valuesz;
  int ret;

  if (!str)
    return NULL;

  opts = g_ptr_array_new_with_free_func (g_free);
  optstr = (char *)str;

  while ((ret = mnt_optstr_next_option (&optstr, &name, &namesz, &value, &valuesz)) == 0)
    {
      gchar *opt;

      if (value == NULL)
        {
          opt = g_strndup (name, namesz);
        }
      else
        {
          opt = g_strdup_printf ("%.*s=%.*s", (int) namesz, name, (int) valuesz, value);
        }
      g_ptr_array_add (opts, opt);
    }
  if (ret < 0)
    {
      udisks_warning ("Malformed mount options string '%s' at position %zd, ignoring",
                      str, optstr - str + 1);
      g_ptr_array_free (opts, TRUE);
      return NULL;
    }

  g_ptr_array_add (opts, NULL);
  return (gchar **) g_ptr_array_free (opts, FALSE);
}

/* transfer-full */
static gchar *
extract_fs_type (const gchar *key, const gchar **group)
{
  if (g_str_equal (key, MOUNT_OPTIONS_KEY_DEFAULTS) ||
      g_str_equal (key, MOUNT_OPTIONS_KEY_ALLOW) ||
      g_str_equal (key, MOUNT_OPTIONS_KEY_ALLOW_UID_SELF) ||
      g_str_equal (key, MOUNT_OPTIONS_KEY_ALLOW_GID_SELF))
    {
      *group = key;
      return g_strdup (MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS);
    }

#define TEST_AND_RETURN_GROUP(g) \
  if (g_str_has_suffix (key, "_" g)) \
    { \
      *group = g; \
      return g_strndup (key, strlen (key) - strlen (g) - 1); \
    }

  TEST_AND_RETURN_GROUP (MOUNT_OPTIONS_KEY_ALLOW_UID_SELF);
  TEST_AND_RETURN_GROUP (MOUNT_OPTIONS_KEY_ALLOW_GID_SELF);
  TEST_AND_RETURN_GROUP (MOUNT_OPTIONS_KEY_ALLOW);
  TEST_AND_RETURN_GROUP (MOUNT_OPTIONS_KEY_DEFAULTS);

  /* invalid key name */
  *group = NULL;
  return NULL;
}

static void
parse_key_value_pair (GHashTable *mount_options, const gchar *key, const gchar *value)
{
  FSMountOptions *ent;
  gchar *fs_type;
  const gchar *group = NULL;
  gchar **opts;

  fs_type = extract_fs_type (key, &group);
  if (!fs_type)
    {
      /* invalid or malformed key detected, do not parse and ignore */
      udisks_debug ("parse_key_value_pair: garbage key found: %s", key);
      return;
    }
  g_warn_if_fail (group != NULL);

  ent = g_hash_table_lookup (mount_options, fs_type);
  if (!ent)
    {
      ent = g_malloc0 (sizeof (FSMountOptions));
      g_hash_table_replace (mount_options, g_strdup (fs_type), ent);
    }

  opts = parse_mount_options_string (value);

#define ASSIGN_OPTS(g,p) \
  if (g_str_equal (group, g)) \
    { \
      if (ent->p) \
        { \
          g_warning ("mount_options_parse_group: Duplicate key '%s' detected", key); \
          g_strfreev (ent->p); \
        } \
      ent->p = opts; \
    } \
  else

  ASSIGN_OPTS (MOUNT_OPTIONS_KEY_ALLOW_UID_SELF, allow_uid_self)
  ASSIGN_OPTS (MOUNT_OPTIONS_KEY_ALLOW_GID_SELF, allow_gid_self)
  ASSIGN_OPTS (MOUNT_OPTIONS_KEY_ALLOW, allow)
  ASSIGN_OPTS (MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS, defaults)
    {
      /* should be caught by extract_fs_type() already */
      g_warning ("parse_key_value_pair: Unmatched key '%s' found, ignoring", key);
    }

  g_free (fs_type);
}

static GHashTable *
mount_options_parse_group (GKeyFile *key_file, const gchar *group_name, GError **error)
{
  GHashTable *mount_options;
  gchar **keys;
  gsize keys_len = 0;

  keys = g_key_file_get_keys (key_file, group_name, &keys_len, error);
  g_warn_if_fail (keys != NULL);

  mount_options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) free_fs_mount_options);
  for (; keys_len > 0; keys_len--)
    {
      gchar *key;
      gchar *value;
      GError *e = NULL;

      key = g_ascii_strdown (keys[keys_len - 1], -1);
      value = g_key_file_get_string (key_file, group_name, keys[keys_len - 1], &e);
      if (!value)
        {
          udisks_warning ("mount_options_parse_group: cannot retrieve value for key '%s': %s",
                          key, e->message);
          g_error_free (e);
        }
      else
        {
          parse_key_value_pair (mount_options, key, value);
        }
      g_free (value);
      g_free (key);
    }

  g_strfreev (keys);

  return mount_options;
}

static GHashTable *
mount_options_parse_key_file (GKeyFile *key_file, GError **error)
{
  GHashTable *mount_options = NULL;
  gchar **groups;
  gsize groups_len = 0;

  groups = g_key_file_get_groups (key_file, &groups_len);
  if (groups == NULL || groups_len == 0)
    {
      g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                           "Failed to parse mount options: No sections found.");
      g_strfreev (groups);
      return NULL;
    }

  mount_options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_hash_table_destroy);
  for (; groups_len > 0; groups_len--)
    {
      GHashTable *opts;
      GError *local_error = NULL;
      gchar *group = groups[groups_len - 1];

      opts = mount_options_parse_group (key_file, group, &local_error);
      if (! opts)
        {
          udisks_warning ("Failed to parse mount options section %s: %s",
                          group, local_error->message);
          g_error_free (local_error);
          /* ignore the whole section, continue with the rest */
        }
      else
        {
          g_hash_table_replace (mount_options, g_strdup (group), opts);
        }
    }
  g_strfreev (groups);

  return mount_options;
}

/* returns two-level hashtable with block specifics at the first level */
static GHashTable *
mount_options_parse_config_file (const gchar *filename, GError **error)
{
  GKeyFile *key_file;
  GHashTable *mount_options;

  key_file = g_key_file_new ();
  if (! g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, error))
    {
      g_key_file_free (key_file);
      return NULL;
    }

  mount_options = mount_options_parse_key_file (key_file, error);
  g_key_file_free (key_file);

  return mount_options;
}

/* returns second level of mount options (not block-specific) */
static GHashTable *
mount_options_get_from_udev (UDisksLinuxDevice *device, GError **error)
{
  GHashTable *mount_options;
  const gchar * const *keys;

  g_warn_if_fail (device != NULL);
  if (!device->udev_device)
    {
      g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                           "'device' is not a valid UDisksLinuxDevice");
      return NULL;
    }

  mount_options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) free_fs_mount_options);

  keys = g_udev_device_get_property_keys (device->udev_device);
  for (; *keys; keys++)
    if (g_str_has_prefix (*keys, UDEV_MOUNT_OPTIONS_PREFIX))
      {
        gchar *key;
        const gchar *value;

        key = g_ascii_strdown (*keys + strlen (UDEV_MOUNT_OPTIONS_PREFIX), -1);
        value = g_udev_device_get_property (device->udev_device, *keys);
        if (!value)
          {
            udisks_warning ("mount_options_get_from_udev: cannot retrieve value for udev property %s",
                            *keys);
          }
        else
          {
            parse_key_value_pair (mount_options, key, value);
          }
        g_free (key);
      }

  return mount_options;
}

/*
 * udisks_linux_mount_options_get_builtin: <internal>
 *
 * Get built-in set of default mount options. This function will never
 * fail, the process is aborted in case of a parse error.
 *
 * Returns: (transfer full) A #GHashTable with mount options.
 */
/* returns two-level hashtable */
GHashTable *
udisks_linux_mount_options_get_builtin (void)
{
  GResource *daemon_resource;
  GBytes *builtin_opts_bytes;
  GKeyFile *key_file;
  GHashTable *mount_options;
  GError *error = NULL;

  daemon_resource = udisks_daemon_resources_get_resource ();
  builtin_opts_bytes = g_resource_lookup_data (daemon_resource,
                                               "/org/freedesktop/UDisks2/data/builtin_mount_options.conf",
                                               G_RESOURCE_LOOKUP_FLAGS_NONE,
                                               &error);
  g_resource_unref (daemon_resource);

  if (builtin_opts_bytes == NULL)
    {
      udisks_error ("Failed to read built-in mount options resource: %s", error->message);
      g_error_free (error);
      return NULL;
    }

  key_file = g_key_file_new ();
  if (! g_key_file_load_from_bytes (key_file, builtin_opts_bytes, G_KEY_FILE_NONE, &error))
    {
      /* should never happen */
      udisks_error ("Failed to read built-in mount options: %s", error->message);
      g_error_free (error);
      g_key_file_free (key_file);
      g_bytes_unref (builtin_opts_bytes);
      return NULL;
    }

  mount_options = mount_options_parse_key_file (key_file, &error);
  g_key_file_free (key_file);
  g_bytes_unref (builtin_opts_bytes);

  if (mount_options == NULL)
    {
      /* should never happen either */
      udisks_error ("Failed to parse built-in mount options: %s", error->message);
      g_error_free (error);
    }
  else if (!g_hash_table_contains (mount_options, MOUNT_OPTIONS_CONFIG_GROUP_DEFAULTS))
    {
      g_hash_table_destroy (mount_options);
      mount_options = NULL;
      udisks_error ("Failed to parse built-in mount options: No global `defaults` section found.");
    }

  return mount_options;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
is_uid_in_gid (uid_t uid,
               gid_t gid)
{
  GError *error = NULL;
  gid_t primary_gid = -1;
  gchar *user_name = NULL;
  static gid_t supplementary_groups[128];
  int num_supplementary_groups = 128;
  int n;

  /* TODO: use some #define instead of harcoding some random number like 128 */

  if (! udisks_daemon_util_get_user_info (uid, &primary_gid, &user_name, &error))
    {
      udisks_warning ("%s", error->message);
      g_error_free (error);
      return FALSE;
    }
  if (primary_gid == gid)
    {
      g_free (user_name);
      return TRUE;
    }

  if (getgrouplist (user_name, primary_gid, supplementary_groups, &num_supplementary_groups) < 0)
    {
      udisks_warning ("Error getting supplementary groups for uid %u: %m", uid);
      g_free (user_name);
      return FALSE;
    }
  g_free (user_name);

  for (n = 0; n < num_supplementary_groups; n++)
    {
      if (supplementary_groups[n] == gid)
        return TRUE;
    }

  return FALSE;
}

#define VARIANT_NULL_STRING  "\1"

static gboolean
is_mount_option_allowed (const FSMountOptions *fsmo,
                         const gchar          *option,
                         const gchar          *value,
                         uid_t                 caller_uid)
{
  gchar *endp;
  uid_t uid;
  gid_t gid;
  gchar *s;

  /* match the exact option=value string within allowed options */
  if (fsmo && fsmo->allow && value && strlen (value) > 0)
    {
      s = g_strdup_printf ("%s=%s", option, value);
      if (g_strv_contains ((const gchar * const *) fsmo->allow, s))
        {
          g_free (s);
          /* not checking whether the option is in UID/GID_self as
           * this is what was explicitly allowed by sysadmin (overrides) */
          return TRUE;
        }
      g_free (s);
    }

  /* .. then check for mount options where the caller is allowed to pass
   * in his own uid
   */
  if (fsmo && fsmo->allow_uid_self &&
      g_strv_contains ((const gchar * const *) fsmo->allow_uid_self, option))
    {
      if (value == NULL || strlen (value) == 0)
        {
          udisks_warning ("is_mount_option_allowed: option '%s' is listed within allow_uid_self but has no value",
                          option);
          return FALSE;
        }
      uid = strtol (value, &endp, 10);
      if (*endp != '\0')
        /* malformed value string */
        return FALSE;
      return (uid == caller_uid);
    }

  /* .. ditto for gid
   */
  if (fsmo && fsmo->allow_gid_self &&
      g_strv_contains ((const gchar * const *) fsmo->allow_gid_self, option))
    {
      if (value == NULL || strlen (value) == 0)
        {
          udisks_warning ("is_mount_option_allowed: option '%s' is listed within allow_gid_self but has no value",
                          option);
          return FALSE;
        }
      gid = strtol (value, &endp, 10);
      if (*endp != '\0')
        /* malformed value string */
        return FALSE;
      return is_uid_in_gid (caller_uid, gid);
    }

  /* the above UID/GID checks also assure that none of the options
   * would be checked again against the _allow array */

  /* match within allowed mount options */
  if (fsmo && fsmo->allow)
    {
      /* check for 'option=' */
      s = g_strdup_printf ("%s=", option);
      if (g_strv_contains ((const gchar * const *) fsmo->allow, s))
        {
          g_free (s);
          return TRUE;
        }
      g_free (s);

      /* simple 'option' match */
      if (g_strv_contains ((const gchar * const *) fsmo->allow, option))
        return TRUE;
    }

  if (g_str_has_prefix (option, "x-"))
    {
      return TRUE;
    }

  return FALSE;
}

static GVariant *
prepend_default_mount_options (const FSMountOptions *fsmo,
                               uid_t                 caller_uid,
                               GVariant             *given_options,
                               gboolean              shared_fs)
{
  GVariantBuilder builder;
  gint n;
  gchar *s;
  gid_t gid;
  const gchar *option_string;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));
  if (fsmo != NULL)
    {
      gchar **defaults = fsmo->defaults;

      for (n = 0; defaults != NULL && defaults[n] != NULL; n++)
        {
          const gchar *option = defaults[n];
          const gchar *eq = strchr (option, '=');

          if (eq != NULL)
            {
              const gchar *value = eq + 1;
              gsize opt_len = eq - option;

              if (value && strlen (value) > 0 && fsmo->allow)
                {
                  /* check that 'option=value' is explicitly allowed */
                  if (g_strv_contains ((const gchar * const *) fsmo->allow, option))
                    {
                      s = g_strndup (option, opt_len);
                      g_variant_builder_add (&builder, "{ss}", s, value);
                      g_free (s);
                      continue;
                    }
                }

              if (strncmp (option, "uid", opt_len) == 0)
                {
                  /* append caller UID */
                  s = g_strdup_printf ("%u", caller_uid);
                  g_variant_builder_add (&builder, "{ss}", "uid", s);
                  g_free (s);
                }
              else if (strncmp (option, "gid", opt_len) == 0)
                {
                  if (udisks_daemon_util_get_user_info (caller_uid, &gid, NULL, NULL))
                    {
                      s = g_strdup_printf ("%u", gid);
                      g_variant_builder_add (&builder, "{ss}", "gid", s);
                      g_free (s);
                    }
                }
              else if (shared_fs && strncmp (option, "mode", opt_len) == 0)
                {
                  /* set different 'mode' and 'dmode' options for file systems mounted at shared
                     location (otherwise they cannot be used by anybody else so mounting them at
                     a shared location doesn't make much sense */
                  gchar *shared_mode = g_strdup (value);

                  /* give group and others the same permissions as to the owner
                     without the 'write' permission, but at least 'read'
                     (HINT: keep in mind that chars are ints in C and that
                     digits are ordered naturally in the ASCII table) */
                  shared_mode[2] = MAX(shared_mode[1] - 2, '4');
                  shared_mode[3] = MAX(shared_mode[1] - 2, '4');
                  g_variant_builder_add (&builder, "{ss}", "mode", shared_mode);
                  g_free (shared_mode);
                }
              else if (shared_fs && strncmp (option, "dmode", opt_len) == 0)
                {
                  /* see right above */
                  /* Does any other dmode than 0555 make sense for a FS mounted
                     at a shared location?  */
                  g_variant_builder_add (&builder, "{ss}", "dmode", "0555");
                }
              else
                {
                  s = g_strndup (option, opt_len);
                  g_variant_builder_add (&builder, "{ss}", s, value);
                  g_free (s);
                }
            }
          else
            g_variant_builder_add (&builder, "{ss}", option, VARIANT_NULL_STRING);
        }
    }

  if (g_variant_lookup (given_options,
                        "options",
                        "&s", &option_string))
    {
      gchar **split_option_string;
      split_option_string = g_strsplit (option_string, ",", -1);
      for (n = 0; split_option_string[n] != NULL; n++)
        {
          gchar *option = split_option_string[n];
          const gchar *eq = strchr (option, '=');

          if (eq != NULL)
            {
              const gchar *value = eq + 1;
              gsize opt_len = eq - option;

              s = g_strndup (option, opt_len);
              g_variant_builder_add (&builder, "{ss}", s, value);
              g_free (s);
            }
          else
            g_variant_builder_add (&builder, "{ss}", option, VARIANT_NULL_STRING);

          g_free (option);
        }
      g_free (split_option_string);
    }

  return g_variant_builder_end (&builder);
}

/* ---------------------------------------------------------------------------------------------------- */

/*
 * udisks_linux_calculate_mount_options: <internal>
 * @daemon: A #UDisksDaemon.
 * @block: A #UDisksBlock.
 * @caller_uid: The uid of the caller making the request.
 * @fs_type: The filesystem type to use or %NULL.
 * @options: Options requested by the caller.
 * @error: Return location for error or %NULL.
 *
 * Calculates the mount option string to use. Ensures (by returning an
 * error) that only safe options are used.
 *
 * Returns: A string with mount options or %NULL if @error is set. Free with g_free().
 */
gchar *
udisks_linux_calculate_mount_options (UDisksDaemon  *daemon,
                                      UDisksBlock   *block,
                                      uid_t          caller_uid,
                                      const gchar   *fs_type,
                                      GVariant      *options,
                                      GError       **error)
{
  FSMountOptions *fsmo;
  GVariant *options_to_use;
  GVariantIter iter;
  UDisksLinuxBlockObject *object = NULL;
  UDisksLinuxDevice *device = NULL;
  gboolean shared_fs = FALSE;
  gchar *options_to_use_str;
  gchar *key, *value;
  gchar *fs_type_l;
  GString *str;

  options_to_use_str = NULL;

  object = udisks_daemon_util_dup_object (block, NULL);
  device = udisks_linux_block_object_get_device (object);
  if (device != NULL && device->udev_device != NULL &&
      g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_FILESYSTEM_SHARED"))
    shared_fs = TRUE;

  fs_type_l = g_ascii_strdown (fs_type, -1);
  fsmo = compute_mount_options_for_fs_type (daemon, block, object, fs_type_l);
  g_free (fs_type_l);

  g_clear_object (&device);
  g_clear_object (&object);

  /* always prepend some reasonable default mount options; these are
   * chosen here; the user can override them if he wants to
   */
  options_to_use = prepend_default_mount_options (fsmo, caller_uid, options, shared_fs);

  /* validate mount options */
  str = g_string_new ("uhelper=udisks2,nodev,nosuid");
  g_variant_iter_init (&iter, options_to_use);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &value))
    {
      /* GVariant doesn't handle NULL strings gracefully */
      if (g_str_equal (value, VARIANT_NULL_STRING))
        value = NULL;
      /* avoid attacks like passing "shortname=lower,uid=0" as a single mount option */
      if (strstr (key, ",") != NULL)
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_OPTION_NOT_PERMITTED,
                       "Malformed mount option `%s'",
                       key);
          g_string_free (str, TRUE);
          goto out;
        }

      /* first check if the mount option is allowed */
      if (!is_mount_option_allowed (fsmo, key, value, caller_uid))
        {
          if (value == NULL)
            {
              g_set_error (error,
                           UDISKS_ERROR,
                           UDISKS_ERROR_OPTION_NOT_PERMITTED,
                           "Mount option `%s' is not allowed",
                           key);
            }
          else
            {
              g_set_error (error,
                           UDISKS_ERROR,
                           UDISKS_ERROR_OPTION_NOT_PERMITTED,
                           "Mount option `%s=%s' is not allowed",
                           key, value);
            }
          g_string_free (str, TRUE);
          goto out;
        }

      g_string_append_c (str, ',');
      if (value == NULL)
        g_string_append (str, key);
      else
        g_string_append_printf (str, "%s=%s", key, value);
    }
  options_to_use_str = g_string_free (str, FALSE);

 out:
  g_variant_unref (options_to_use);
  free_fs_mount_options (fsmo);

  g_assert (options_to_use_str == NULL || g_utf8_validate (options_to_use_str, -1, NULL));

  return options_to_use_str;
}

/* ---------------------------------------------------------------------------------------------------- */
