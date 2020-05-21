/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>

#include <glib.h>

#include "ostree-mount-util.h"
#include "glnx-backport-autocleanups.h"

static void
do_remount (const char *target,
            bool        writable)
{
  struct stat stbuf;
  if (lstat (target, &stbuf) < 0)
    return;
  /* Silently ignore symbolic links; we expect these to point to
   * /sysroot, and thus there isn't a bind mount there.
   */
  if (S_ISLNK (stbuf.st_mode))
    return;
  /* If not a mountpoint, skip it */
  struct statvfs stvfsbuf;
  if (statvfs (target, &stvfsbuf) == -1)
    return;

  const bool currently_writable = ((stvfsbuf.f_flag & ST_RDONLY) == 0);
  if (writable == currently_writable)
    return;

  int mnt_flags = MS_REMOUNT | MS_SILENT;
  if (!writable)
    mnt_flags |= MS_RDONLY;
  if (mount (target, target, NULL, mnt_flags, NULL) < 0)
    {
      /* Also ignore EINVAL - if the target isn't a mountpoint
       * already, then assume things are OK.
       */
      if (errno != EINVAL)  
        err (EXIT_FAILURE, "failed to remount(%s) %s", writable ? "rw" : "ro", target);
      else
        return;
    }

  printf ("Remounted %s: %s\n", writable ? "rw" : "ro", target);
}

static bool
sysroot_is_configured_ro (void)
{
  struct stat stbuf;
  static const char config_path[] = "/ostree/repo/config";
  if (stat (config_path, &stbuf) != 0)
    return false;

  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, config_path, 0, NULL))
    return false;

  if (g_key_file_get_boolean (keyfile, "sysroot", "readonly", NULL))
    puts ("Ignoring sysroot.readonly config; see https://github.com/coreos/fedora-coreos-tracker/issues/488.");

  return false;
}

int
main(int argc, char *argv[])
{
  /* When systemd is in use this is normally created via the generator, but
   * we ensure it's created here as well for redundancy.
   */
  touch_run_ostree ();

  /* The /sysroot mount needs to be private to avoid having a mount for e.g. /var/cache
   * also propagate to /sysroot/ostree/deploy/$stateroot/var/cache
   *
   * Today systemd remounts / (recursively) as shared, so we're undoing that as early
   * as possible.  See also a copy of this in ostree-prepare-root.c.
   */
  if (mount ("none", "/sysroot", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    perror ("warning: While remounting /sysroot MS_PRIVATE");

  if (path_is_on_readonly_fs ("/"))
    {
      /* If / isn't writable, don't do any remounts; we don't want
       * to clear the readonly flag in that case.
       */
      exit (EXIT_SUCCESS);
    }

  /* Query the repository configuration - this is an operating system builder
   * choice.
   * */
  const bool sysroot_readonly = sysroot_is_configured_ro ();

  /* Mount the sysroot read-only if we're configured to do so.
   * Note we only get here if / is already writable.
   */
  do_remount ("/sysroot", !sysroot_readonly);

  if (sysroot_readonly)
    {
      /* Now, /etc is not normally a bind mount, but remounting the
       * sysroot above made it read-only since it's on the same filesystem.
       * Make it a self-bind mount, so we can then mount it read-write.
       */
      if (mount ("/etc", "/etc", NULL, MS_BIND, NULL) < 0)
        err (EXIT_FAILURE, "failed to make /etc a bind mount");
      do_remount ("/etc", true);
    }

  /* If /var was created as as an OSTree default bind mount (instead of being a separate filesystem)
    * then remounting the root mount read-only also remounted it.
    * So just like /etc, we need to make it read-write by default.
    * If it was a separate filesystem, we expect it to be writable anyways,
    * so it doesn't hurt to remount it if so.
    *
    * And if we started out with a writable system root, then we need
    * to ensure that the /var bind mount created by the systemd generator
    * is writable too.
    */
  do_remount ("/var", true);

  exit (EXIT_SUCCESS);
}
