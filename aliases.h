/* Copyright (C) 1996, 1997 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the GNU C Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#ifndef _ALIASES_H

#define _ALIASES_H      1
#include <features.h>

#include <sys/types.h>

__BEGIN_DECLS

/* Structure to represent one entry of the alias data base.  */
struct aliasent
  {
    char *alias_name;
    size_t alias_members_len;
    char **alias_members;
    int alias_local;
  };


/* Open alias data base files.  */
extern void setaliasent __P ((void));

/* Close alias data base files.  */
extern void endaliasent __P ((void));

/* Get the next entry from the alias data base.  */
extern struct aliasent *getaliasent __P ((void));

/* Get the next entry from the alias data base and put it in RESULT_BUF.  */
extern int getaliasent_r __P ((struct aliasent * __result_buf, char *__buffer,
			     size_t __buflen, struct aliasent ** __result));

/* Get alias entry corresponding to NAME.  */
extern struct aliasent *getaliasbyname __P ((__const char *__name));

/* Get alias entry corresponding to NAME and put it in RESULT_BUF.  */
extern int getaliasbyname_r __P ((__const char *__name,
				  struct aliasent * __result_buf,
				  char *__buffer, size_t __buflen,
				  struct aliasent ** __result));

__END_DECLS

#endif /* aliases.h */
