/* Copyright (C) 1997 Luke Howard.
   This file is part of the nss_ldap library.
   Contributed by Luke Howard, <lukeh@padl.com>, 1997.

   The nss_ldap library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The nss_ldap library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the nss_ldap library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
 */

static char rcsId[] = "$Id$";

#ifdef IRS_NSS

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <lber.h>
#include <ldap.h>
#include <errno.h>

#include "irs-nss.h"
#include "ldap-nss.h"
#include "ltf.h"
#include "globals.h"
#include "util.h"

static void
  ldap_close (struct irs_acc *this);

/* Dispatch table for IRS LDAP module */

struct irs_acc *
irs_ldap_acc (const char *options)
{
  struct irs_acc *acc;

  if (!(acc = malloc (sizeof (*acc))))
    {
      errno = ENOMEM;
      return NULL;
    }

  memset (acc, 0x5e, sizeof *acc);

  /* private stuff gets kept as static in ldap-nss.c. */
  acc->private = NULL;

  acc->gr_map = irs_ldap_gr;
#ifdef WANT_IRS_PW
  acc->pw_map = irs_ldap_pw;
#endif
  acc->sv_map = irs_ldap_sv;
  acc->pr_map = irs_ldap_pr;
  acc->ho_map = irs_ldap_ho;
  acc->nw_map = irs_ldap_nw;

  /* haven't done netgroups yet */
#if 0
  acc->ng_map = irs_ldap_ng;
#else
  acc->ng_map = NULL;
#endif

  acc->close = ldap_close;

  return (acc);
}

/* Methods */

static void
ldap_close (struct irs_acc *this)
{
  free (this);
}
#endif
