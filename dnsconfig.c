/* Copyright (C) 1997-2003 Luke Howard.
   This file is part of the nss_ldap library.
   Contributed by Luke Howard, <lukeh@padl.com>, 1997.
   (The author maintains a non-exclusive licence to distribute this file
   under their own conditions.)

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

/*
 * Support DNS SRV records. I look up the SRV record for
 * _ldap._tcp.gnu.org.
 * and build the DN DC=gnu,DC=org.
 * Thanks to Assar & co for resolve.[ch].
 */

static char rcsId[] =
  "$Id$";

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <netdb.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <string.h>

#ifdef HAVE_LBER_H
#include <lber.h>
#endif
#ifdef HAVE_LDAP_H
#include <ldap.h>
#endif

#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif

#include "ldap-nss.h"
#include "util.h"
#include "resolve.h"
#include "dnsconfig.h"

/* map gnu.org into DC=gnu,DC=org */
NSS_STATUS
_nss_ldap_getdnsdn (char *src_domain,
		    char **rval, char **buffer, size_t * buflen)
{
  char *p;
  int len = 0;
#ifdef HAVE_STRTOK_R
  char *st = NULL;
#endif
  char *bptr;
  char *domain, *domain_copy;

  /* we need to take a copy of domain, because strtok() modifies
   * it in place. Bad.
   */
  domain_copy = strdup (src_domain);
  if (domain_copy == NULL)
    {
      return NSS_TRYAGAIN;
    }

  domain = domain_copy;

  bptr = *rval = *buffer;
  **rval = '\0';

#ifndef HAVE_STRTOK_R
  while ((p = strtok (domain, ".")))
#else
  while ((p = strtok_r (domain, ".", &st)))
#endif
    {
      len = strlen (p);

      if (*buflen < (size_t) (len + DC_ATTR_AVA_LEN + 1 /* D C = [,|\0] */ ))
	{
	  free (domain_copy);
	  return NSS_TRYAGAIN;
	}

      if (domain == NULL)
	{
	  strcpy (bptr, ",");
	  bptr++;
	}
      else
	{
	  domain = NULL;
	}

      strcpy (bptr, DC_ATTR_AVA);
      bptr += DC_ATTR_AVA_LEN;

      strcpy (bptr, p);
      bptr += len;		/* don't include comma */
      *buffer += len + DC_ATTR_AVA_LEN + 1;
      *buflen -= len + DC_ATTR_AVA_LEN + 1;
    }

  if (bptr != NULL)
    {
      (*rval)[bptr - *rval] = '\0';
    }

  free (domain_copy);

  return NSS_SUCCESS;
}

NSS_STATUS
_nss_ldap_readconfigfromdns (ldap_config_t ** presult,
			     char *buffer, size_t buflen)
{
  NSS_STATUS stat = NSS_SUCCESS;
  struct dns_reply *r;
  struct resource_record *rr;
  char domain[MAXHOSTNAMELEN + 1];
  ldap_config_t *result = NULL;

  if ((_res.options & RES_INIT) == 0 && res_init () == -1)
    {
      return NSS_UNAVAIL;
    }

  snprintf (domain, sizeof (domain), "_ldap._tcp.%s.", _res.defdname);

  r = dns_lookup (domain, "srv");
  if (r == NULL)
    {
      return NSS_NOTFOUND;
    }

  /* XXX sort by priority */
  for (rr = r->head; rr != NULL; rr = rr->next)
    {
      if (rr->type == T_SRV)
	{
	  int len;
	  ldap_config_t *last = result;

	  if (bytesleft (buffer, buflen, ldap_config_t *) <
	      sizeof (ldap_config_t))
	    {
	      dns_free_data (r);
	      return NSS_TRYAGAIN;
	    }
	  align (buffer, buflen, ldap_config_t *);
	  result = (ldap_config_t *) buffer;
	  buffer += sizeof (ldap_config_t);
	  buflen -= sizeof (ldap_config_t);
	  _nss_ldap_init_config (result);
	  if (last != NULL)
	    {
	      last->ldc_next = result;
	    }
	  else
	    {
	      *presult = result;
	    }

	  len = strlen (rr->u.srv->target);
	  if (buflen < (size_t) (len + 1))
	    {
	      dns_free_data (r);
	      return NSS_TRYAGAIN;
	    }
	  /* Server Host */
	  memcpy (buffer, rr->u.srv->target, len + 1);
	  result->ldc_host = buffer;
	  buffer += len + 1;
	  buflen -= len + 1;

	  /* Port */
	  result->ldc_port = rr->u.srv->port;
#ifdef LDAPS_PORT
	  /* Hack: if the port is the registered SSL port, enable SSL. */
	  if (result->ldc_port == LDAPS_PORT)
	    {
	      result->ldc_ssl_on = SSL_LDAPS;
	    }
#endif /* SSL */

	  /* DN */
	  stat = _nss_ldap_getdnsdn (_res.defdname,
				     &result->ldc_base, &buffer, &buflen);
	  if (stat != NSS_SUCCESS)
	    {
	      dns_free_data (r);
	      return stat;
	    }
	}
    }

  dns_free_data (r);
  stat = NSS_SUCCESS;

  return stat;
}
