
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
#include <port_before.h>
#endif

#ifdef SUN_NSS
#include <thread.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <grp.h>
#include <lber.h>
#include <ldap.h>

#ifdef GNU_NSS
#include <nss.h>
#elif defined(SUN_NSS)
#include <nss_common.h>
#include <nss_dbdefs.h>
#include <nsswitch.h>
#endif

#include "ldap-nss.h"
#include "ldap-grp.h"
#include "globals.h"
#include "util.h"

#ifdef IRS_NSS
#include <port_after.h>
#endif

#ifdef GNU_NSS
static context_handle_t gr_context = NULL;
#endif

static NSS_STATUS 
_nss_ldap_parse_gr (
		     LDAP * ld,
		     LDAPMessage * e,
		     ldap_state_t * pvt,
		     void *result,
		     char *buffer,
		     size_t buflen)
{
  struct group *gr = (struct group *) result;
  char *gid;
  NSS_STATUS stat;
#ifdef RFC2307BIS
  char **uid_mems, **dn_mems, **vals;
  int uid_mems_c = 0, dn_mems_c = 0;
#endif /* RFC2307BIS */

  stat = _nss_ldap_assign_attrval (ld, e, LDAP_ATTR_GROUP_GID, &gid, &buffer, &buflen);
  if (stat != NSS_SUCCESS)
    return stat;

  gr->gr_gid = (*gid == '\0') ? GID_NOBODY : (gid_t) atol (gid);

  stat = _nss_ldap_assign_attrval (ld, e, LDAP_ATTR_GROUPNAME, &gr->gr_name, &buffer, &buflen);
  if (stat != NSS_SUCCESS)
    return stat;

  stat = _nss_ldap_assign_passwd (ld, e, LDAP_ATTR_GPASSWD, &gr->gr_passwd, &buffer, &buflen);
  if (stat != NSS_SUCCESS)
    return stat;

#ifndef RFC2307BIS
  stat = _nss_ldap_assign_attrvals (ld, e, LDAP_ATTR_UIDMEMBERS, NULL, &gr->gr_mem, &buffer, &buflen, NULL);
  if (stat != NSS_SUCCESS)
    return stat;
#else
  dn_mems = NULL;
  vals = ldap_get_values (ld, e, LDAP_ATTR_DNMEMBERS);
  if (vals != NULL)
    {
      char **mem_p, **valiter;

      dn_mems_c = ldap_count_values (vals);

      if (bytesleft (buffer, buflen) < (dn_mems_c + 1) * sizeof (char *))
	{
	  ldap_value_free (vals);
	  return NSS_TRYAGAIN;
	}
      align (buffer, buflen);
      mem_p = dn_mems = (char **) buffer;
      buffer += (dn_mems_c + 1) * sizeof (char *);
      buflen -= (dn_mems_c + 1) * sizeof (char *);
      for (valiter = vals; *valiter != NULL; valiter++)
	{
	  char *uid;
	  stat = _nss_ldap_dn2uid (ld, *valiter, &uid, &buffer, &buflen);
	  if (stat == NSS_SUCCESS)
	    {
	      *mem_p = uid;
	      mem_p++;
	    }
	  else
	    dn_mems_c--;
	}
      ldap_value_free (vals);
    }

  stat = _nss_ldap_assign_attrvals (ld, e, LDAP_ATTR_UIDMEMBERS, NULL, &uid_mems, &buffer, &buflen, &uid_mems_c);
  if (stat != NSS_SUCCESS)
    uid_mems = NULL;

  if (dn_mems == NULL)
    {
      if (uid_mems == NULL)
	gr->gr_mem = NULL;
      else
	gr->gr_mem = uid_mems;
    }
  else
    {
      if (uid_mems == NULL)
	gr->gr_mem = dn_mems;
      else
	{
	  if (bytesleft (buffer, buflen) < (dn_mems_c + uid_mems_c + 1) * sizeof (char *))
	      return NSS_TRYAGAIN;
	  align (buffer, buflen);
	  gr->gr_mem = (char **) buffer;
	  buffer += (dn_mems_c + uid_mems_c + 1) * sizeof (char *);
	  buflen -= (dn_mems_c + uid_mems_c + 1) * sizeof (char *);
	  memcpy (gr->gr_mem, dn_mems, (dn_mems_c * sizeof (char *)));
	  memcpy (&gr->gr_mem[dn_mems_c], uid_mems, (uid_mems_c * sizeof (char *)));
	  gr->gr_mem[dn_mems_c + uid_mems_c] = NULL;
	}
    }
#endif /* RFC2307BIS */

  return NSS_SUCCESS;
}

#ifdef SUN_NSS
static NSS_STATUS 
_nss_ldap_getgroupsbymember_r (nss_backend_t * be, void *args)
{
  /* filt_getgroupsbymember */

  return NSS_NOTFOUND;
}
#endif /* SUN_NSS */

#ifdef GNU_NSS
NSS_STATUS 
_nss_ldap_getgrnam_r (
		       const char *name,
		       struct group * result,
		       char *buffer,
		       size_t buflen,
		       int *errnop)
{
  LOOKUP_NAME (name, result, buffer, buflen, errnop, filt_getgrnam, gr_attributes, _nss_ldap_parse_gr);
}
#elif defined(SUN_NSS)
static NSS_STATUS 
_nss_ldap_getgrnam_r (nss_backend_t * be, void *args)
{
  LOOKUP_NAME (args, filt_getgrnam, gr_attributes, _nss_ldap_parse_gr);
}
#endif

#ifdef GNU_NSS
NSS_STATUS 
_nss_ldap_getgrgid_r (
		       gid_t gid,
		       struct group *result,
		       char *buffer,
		       size_t buflen,
		       int *errnop)
{
  LOOKUP_NUMBER (gid, result, buffer, buflen, errnop, filt_getgrgid, gr_attributes, _nss_ldap_parse_gr);
}
#elif defined(SUN_NSS)
static NSS_STATUS 
_nss_ldap_getgrgid_r (nss_backend_t * be, void *args)
{
  LOOKUP_NUMBER (args, key.gid, filt_getgrgid, gr_attributes, _nss_ldap_parse_gr);
}
#endif

#if defined(GNU_NSS)
NSS_STATUS 
_nss_ldap_setgrent (void)
{
  LOOKUP_SETENT (gr_context);
}
#elif defined(SUN_NSS)
static NSS_STATUS 
_nss_ldap_setgrent_r (nss_backend_t * gr_context, void *args)
{
  LOOKUP_SETENT (gr_context);
}
#endif

#if defined(GNU_NSS)
NSS_STATUS 
_nss_ldap_endgrent (void)
{
  LOOKUP_ENDENT (gr_context);
}
#elif defined(SUN_NSS)
static NSS_STATUS 
_nss_ldap_endgrent_r (nss_backend_t * gr_context, void *args)
{
  LOOKUP_ENDENT (gr_context);
}
#endif

#ifdef GNU_NSS
NSS_STATUS 
_nss_ldap_getgrent_r (
		       struct group *result,
		       char *buffer,
		       size_t buflen,
		       int *errnop)
{
  LOOKUP_GETENT (gr_context, result, buffer, buflen, errnop, filt_getgrent, gr_attributes, _nss_ldap_parse_gr);
}
#elif defined(SUN_NSS)
static NSS_STATUS 
_nss_ldap_getgrent_r (nss_backend_t * gr_context,
		      void *args)
{
  LOOKUP_GETENT (args, gr_context, filt_getgrent, gr_attributes, _nss_ldap_parse_gr);
}
#endif

#ifdef SUN_NSS
static NSS_STATUS 
_nss_ldap_group_destr (nss_backend_t * gr_context, void *args)
{
  return _nss_ldap_default_destr (gr_context, args);
}

static nss_backend_op_t group_ops[] =
{
  _nss_ldap_group_destr,
  _nss_ldap_endgrent_r,
  _nss_ldap_setgrent_r,
  _nss_ldap_getgrent_r,
  _nss_ldap_getgrnam_r,
  _nss_ldap_getgrgid_r
/*      _nss_ldap_getgroupsbymember_r   */
};

nss_backend_t *
_nss_ldap_group_constr (const char *db_name,
			const char *src_name,
			const char *cfg_args)
{
  nss_ldap_backend_t *be;

  if (!(be = (nss_ldap_backend_t *) malloc (sizeof (*be))))
    return NULL;

  be->ops = group_ops;
  be->n_ops = sizeof (group_ops) / sizeof (nss_backend_op_t);

  /* a NOOP at the moment */
  if (_nss_ldap_default_constr (be) != NSS_SUCCESS)
    return NULL;

  return (nss_backend_t *) be;
}


#endif /* !GNU_NSS */

#ifdef IRS_NSS
#include "irs-grp.c"
#endif
