
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

   $Id$
 */

#ifndef _LDAP_NSS_LDAP_LDAP_ALIAS_H
#define _LDAP_NSS_LDAP_LDAP_ALIAS_H

#define LDAP_CLASS_ALIAS                "nisMailAlias"
#define LDAP_ATTR_ALIASNAME             "cn"
#define LDAP_ATTR_MEMBERS               "rfc822MailMember"

static const char *alias_attributes[] =
{LDAP_ATTR_ALIASNAME, LDAP_ATTR_MEMBERS, NULL};

static const char filt_getaliasbyname[] =
"(&(objectclass=" LDAP_CLASS_ALIAS ")(" LDAP_ATTR_ALIASNAME "=%s))";
static const char filt_getaliasent[] =
"(objectclass=" LDAP_CLASS_ALIAS ")";

static NSS_STATUS _nss_ldap_parse_alias (LDAP * ld,
					 LDAPMessage * e,
					 ldap_state_t *,
					 void *result,
					 char *buffer,
					 size_t buflen);

#if 0
/* no support in Sun NSS for aliases */

static NSS_STATUS _nss_ldap_getaliasbyname_r (nss_backend_t * be, void *fakeargs);
static NSS_STATUS _nss_ldap_getaliasent_r (nss_backend_t * be, void *fakeargs);
static NSS_STATUS _nss_ldap_setaliasent_r (nss_backend_t * be, void *fakeargs);
static NSS_STATUS _nss_ldap_endaliasent_r (nss_backend_t * be, void *fakeargs);

nss_backend_t *_nss_ldap_alias_constr (const char *db_name,
				       const char *src_name,
				       const char *cfg_args);
#endif

#endif /* _LDAP_NSS_LDAP_LDAP_ALIAS_H */
