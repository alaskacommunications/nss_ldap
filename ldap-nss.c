
/* Copyright (C) 1997, 1998, 1999, 2000 Luke Howard.
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

static char rcsId[] =
  "$Id$";

#ifdef SUN_NSS
#include <thread.h>
#endif

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <lber.h>
#include <ldap.h>
#ifdef SSL
#include <ldap_ssl.h>
#endif /* SSL */

#ifdef GNU_NSS
#include <nss.h>
#elif defined(IRS_NSS)
#include "irs-nss.h"
#elif defined(SUN_NSS)
#include <nss_common.h>
#include <nss_dbdefs.h>
#include <nsswitch.h>
#endif

#include "ldap-nss.h"
#include "ltf.h"
#include "globals.h"
#include "util.h"
#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif /* HAVE_SNPRINTF */
#include "dnsconfig.h"

/*
 * If things don't link because ldap_ld_free() isn't defined,
 * then try undefining this. I think it is exported on
 * Linux but not Solaris with Netscape's C SDK.
 *
 */
#define HAVE_LDAP_LD_FREE

/* how many messages to retrieve results for */
#ifndef LDAP_MSG_ONE
#define LDAP_MSG_ONE            0x00
#endif
#ifndef LDAP_MSG_ALL
#define LDAP_MSG_ALL            0x01
#endif
#ifndef LDAP_MSG_RECEIVED
#define LDAP_MSG_RECEIVED       0x02
#endif

/*
 * the configuration is read by the first call to do_open().
 * Pointers to elements of the list are passed around but should not
 * be freed.
 */
static char __configbuf[NSS_LDAP_CONFIG_BUFSIZ];
static ldap_config_t *__config = NULL;

/*
 * Global LDAP session.
 */
static ldap_session_t __session = { NULL, NULL };

/* 
 * Process ID that opened the session.
 */
static pid_t __pid = -1;
static uid_t __euid = -1;

#ifdef SSL
static int __ssl_initialized = 0;
#endif /* SSL */
/*
 * Close the global session, sending an unbind.
 */
static void do_close (void);

/*
 * Close the global session without sending an unbind.
 */
static void do_close_no_unbind (void);

#ifdef DISABLE_SO_KEEPALIVE
/*
 * Disable keepalive on a LDAP connection's socket.
 */
static void do_disable_keepalive (LDAP * ld);
#endif

/*
 * Open the global session
 */
static NSS_STATUS do_open (void);

/*
 * Perform an asynchronous search.
 */
static NSS_STATUS do_search (const char *base, int scope,
			     const char *filter, const char **attrs,
			     int sizelimit, int *);

/*
 * Perform a synchronous search (layered on do_search()).
 */
static NSS_STATUS do_search_s (const char *base, int scope,
			       const char *filter, const char **attrs,
			       int sizelimit, LDAPMessage **);

/*
 * Fetch an LDAP result.
 */
static NSS_STATUS do_result (ent_context_t * ctx, int all);

/*
 * Format a filter given a prototype.
 */
static NSS_STATUS do_filter (const ldap_args_t * args, const char *filterprot,
		       const char **attrs, char *filter, size_t filterlen);

/*
 * Parse a result, fetching new results until a successful parse
 * or exceptional condition.
 */
static NSS_STATUS do_parse (ent_context_t * ctx, void *result, char *buffer,
			    size_t buflen, int *errnop, parser_t parser);

/*
 * Function to be braced by reconnect harness. Used so we
 * can apply the reconnect code to both asynchronous and
 * synchronous searches.
 */
typedef NSS_STATUS (*search_func_t) (const char *, int, const char *,
				     const char **, int, void *);

/*
 * Do a search with a reconnect harness.
 */
static NSS_STATUS
do_with_reconnect (const char *base, int scope,
		   const char *filter, const char **attrs, int sizelimit,
		   void *private, search_func_t func);

/*
 * Do a bind with a defined timeout
 */
static int
do_bind (LDAP *ld, const char *dn, const char *pw);


/*
 * Rebind functions.
 */
#if NETSCAPE_API_EXTENSIONS
static int
_nss_ldap_rebind (LDAP * ld, char **whop, char **credp, int *methodp,
		  int freeit, void *arg)
#else
static int
_nss_ldap_rebind (LDAP * ld, char **whop, char **credp, int *methodp,
		  int freeit)
#endif				/* NETSCAPE_API_EXTENSIONS */
{
  if (freeit)
    {
      if (*whop != NULL)
	free (*whop);
      if (*credp != NULL)
	free (*credp);
    }

  *whop = *credp = NULL;
  if (geteuid() == 0 && __session.ls_config->ldc_rootbinddn)
  {
    *whop = strdup (__session.ls_config->ldc_rootbinddn);
    if (__session.ls_config->ldc_rootbindpw)
      *credp = strdup (__session.ls_config->ldc_rootbindpw);
  }
  else
  {
    if (__session.ls_config->ldc_binddn != NULL)
      *whop = strdup (__session.ls_config->ldc_binddn);
    if (__session.ls_config->ldc_bindpw != NULL)
      *credp = strdup (__session.ls_config->ldc_bindpw);
  }

  *methodp = LDAP_AUTH_SIMPLE;

  return LDAP_SUCCESS;
}

#ifdef SUN_NSS
/*
 * Default destructor.
 * The entry point for this function is the destructor in the dispatch
 * table for the switch. Thus, it's safe to grab the mutex from this
 * function.
 */
NSS_STATUS _nss_ldap_default_destr (nss_backend_t * be, void *args)
{
  debug ("==> _nss_ldap_default_destr");

  if ((((nss_ldap_backend_t *) be)->state) != NULL)
    {
      _nss_ldap_ent_context_free ((ent_context_t **)
				  (&((nss_ldap_backend_t *) be)->state));
    }

  /* Ditch the backend. */
  free (be);

  debug ("<== _nss_ldap_default_destr");

  return NSS_SUCCESS;
}

/*
 * This is the default "constructor" which gets called from each 
 * constructor, in the NSS dispatch table.
 */
NSS_STATUS _nss_ldap_default_constr (nss_ldap_backend_t * be)
{
  debug ("==> _nss_ldap_default_constr");

  be->state = NULL;

  debug ("<== _nss_ldap_default_constr");

  return NSS_SUCCESS;
}
#endif /* SUN_NSS */

/*
 * Closes connection to the LDAP server.
 * This assumes that we have exclusive access to __session.ls_conn,
 * either by some other function having acquired a lock, or by
 * using a thread safe libldap.
 */
static void
do_close (void)
{
  debug ("==> do_close");

  if (__session.ls_conn != NULL)
    {
#ifdef DEBUG
      syslog (LOG_DEBUG, "nss_ldap: closing connection %p",
	      __session.ls_conn);
#endif /* DEBUG */
      ldap_unbind (__session.ls_conn);
      __session.ls_conn = NULL;
    }

  debug ("<== do_close");
}

/*
 * If we've forked, then we need to open a new session.
 * Careful: we have the socket shared with our parent,
 * so we don't want to send an unbind to the server.
 * However, we want to close the descriptor to avoid
 * leaking it, and we also want to release the memory
 * used by __session.ls_conn. The only entry point
 * we have is ldap_unbind() which does both of these
 * things, so we use an internal API, at the expense
 * of compatibility.
 */
static void
do_close_no_unbind (void)
{
  debug ("==> do_close_no_unbind");

  if (__session.ls_conn != NULL)
    {
#ifdef DEBUG
      syslog (LOG_DEBUG, "nss_ldap: closing connection (no unbind) %p",
	      __session.ls_conn);
#endif /* DEBUG */
    }

  if (__session.ls_conn != NULL)
    {
#ifdef HAVE_LDAP_LD_FREE

# if defined(LDAP_API_FEATURE_X_OPENLDAP) && (LDAP_API_VERSION > 2000)
      extern int ldap_ld_free (LDAP * ld, int close, LDAPControl **,
			       LDAPControl **);
      (void) ldap_ld_free (__session.ls_conn, 0, NULL, NULL);
# else
      extern int ldap_ld_free (LDAP * ld, int close);
      (void) ldap_ld_free (__session.ls_conn, 0);
# endif				/* OPENLDAP 2.x */

#else
      /*
       * We'll be rude and close the socket ourselves. 
       * XXX untested code
       */
      int sd = -1;
# ifdef LDAP_VERSION3_API
      if (ldap_get_option (__session.ls_conn, LDAP_OPT_DESC, &sd) == 0)
# else
	if ((sd = __session.ls_conn->ld_sb.sb_sd) > 0)
# endif				/* LDAP_VERSION3_API */
	  {
	    close (sd);
	    sd = -1;
# ifdef LDAP_VERSION3_API
	    (void) ldap_set_option (__session.ls_conn, LDAP_OPT_DESC, &sd);
# else
	    __session.ls_conn->ld_sb.sb_sd = sd;
# endif				/*  LDAP_VERSION3_API */
	  }

      /* hope we closed it OK! */
      ldap_unbind (__session.ls_conn);

#endif /* HAVE_LDAP_LD_FREE */

      __session.ls_conn = NULL;
    }
  debug ("<== do_close_no_unbind");

  return;
}

#ifdef DISABLE_SO_KEEPALIVE
static INLINE void
do_disable_keepalive (LDAP * ld)
{
  int sd = -1;

  debug ("==> do_disable_keepalive");
#ifdef LDAP_VERSION3_API
  if (ldap_get_option (ld, LDAP_OPT_DESC, &sd) == 0)
#else
  if ((sd = ld->ld_sb.sb_sd) > 0)
#endif /* LDAP_VERSION3_API */
    {
      int off = 0;
      (void) setsockopt (sd, SOL_SOCKET, SO_KEEPALIVE, &off, sizeof off);
    }
  debug ("<== do_disable_keepalive");

  return;
}
#endif /* DISABLE_SO_KEEPALIVE */

/*
 * Opens connection to an LDAP server.
 * As with do_close(), this assumes ownership of sess.
 * It also wants to own __config: is there a potential deadlock here? XXX
 */
static NSS_STATUS
do_open (void)
{
  ldap_config_t *cfg = NULL;
  pid_t pid;
  uid_t euid;

  debug ("==> do_open");

  euid = geteuid ();
  pid = getpid ();

#ifdef DEBUG
  syslog (LOG_DEBUG,
	  "nss_ldap: __session.ls_conn=%p, __pid=%i, pid=%i, __euid=%i, euid=%i",
	  __session.ls_conn, __pid, pid, __euid, euid);
#endif /* DEBUG */


  if (__pid != pid)
    {
      do_close_no_unbind ();
    }
  else if (__euid != euid && (__euid == 0 || euid == 0))
    {
      /*
       * If we've changed user ids, close the session so we can
       * rebind as the correct user.
       */
      do_close ();
    }
  else if (__session.ls_conn != NULL && __session.ls_config != NULL)
    {
#ifndef SSL
      /*
       * Otherwise we can hand back this process' global
       * LDAP session.
       *
       * Ensure we save signal handler for sigpipe and restore after
       * LDAP connection is confirmed to be up or a new connection
       * is opened. This prevents Solaris nscd and other apps from
       * dying on a SIGPIPE. I'm not entirely convinced that we
       * ought not to block SIGPIPE elsewhere, but at least this is
       * the entry point where connections get woken up.
       *
       * Also: this may not be necessary now that keepaliave is
       * disabled (the signal code that is). 
       *
       * The W2K client library sends an ICMP echo to the server to
       * check it is up. Perhaps we shold do the same.
       */
      struct sockaddr_in sin;
      int sd = -1;

#ifdef LDAP_VERSION3_API
      if (ldap_get_option (__session.ls_conn, LDAP_OPT_DESC, &sd) == 0)
#else
      if ((sd = __session.ls_conn->ld_sb.sb_sd) > 0)
#endif /* LDAP_VERSION3_API */
	{
	  void (*old_handler) (int sig);
	  size_t len;

#ifdef SUN_NSS
	  old_handler = sigset (SIGPIPE, SIG_IGN);
#else
	  old_handler = signal (SIGPIPE, SIG_IGN);
#endif /* SUN_NSS */
	  len = sizeof(sin);
	  if (getpeername (sd, (struct sockaddr *) &sin, &len) < 0)
	    {
	      /*
	       * The other end has died. Close the connection.
	       */
	      debug ("other end dead!\n");
	      do_close ();
	    }
	  if (old_handler != SIG_ERR && old_handler != SIG_IGN)
	    {
#ifdef SUN_NSS
	      (void) sigset (SIGPIPE, old_handler);
#else
	      (void) signal (SIGPIPE, old_handler);
#endif /* SUN_NSS */
	    }
	}
#else
      /* XXX Permanently ignore SIGPIPE. */
#ifdef SUN_NSS
      (void) sigset (SIGPIPE, SIG_IGN);
#else
      (void) signal (SIGPIPE, SIG_IGN);
#endif /* SUN_NSS */
#endif /* SSL */
      /*
       * If the connection is still there (ie. do_close() wasn't
       * called) then we can return the cached connection.
       */
      if (__session.ls_conn != NULL)
	{
	  debug ("<== do_open");
	  return NSS_SUCCESS;
	}
    }

  __pid = pid;
  __euid = euid;
  __session.ls_config = NULL;

  if (__config == NULL)
    {
      NSS_STATUS status;

      status =
	_nss_ldap_readconfig (&__config, __configbuf, sizeof (__configbuf));

      if (status != NSS_SUCCESS)
	{
	  status =
	    _nss_ldap_readconfigfromdns (&__config, __configbuf,
					 sizeof (__configbuf));
	}

      if (status != NSS_SUCCESS)
	{
	  __config = NULL;
	  debug ("<== do_open");
	  return status;
	}
    }

  cfg = __config;

  while (1)
    {
#ifdef SSL
      /*
       * Initialize the SSL library. 
       */
      if (cfg->ldc_ssl_on)
	{
	  if (__ssl_initialized == 0 && ldapssl_client_init (cfg->ldc_sslpath, NULL) != LDAP_SUCCESS)
	    {
	      continue;
	    }
	  __ssl_initialized = 1;
	}
#endif /* SSL */

#ifdef LDAP_VERSION3_API
      debug ("==> ldap_init");
      __session.ls_conn = ldap_init (cfg->ldc_host, cfg->ldc_port);
      debug ("<== ldap_init");
#else
      debug ("==> ldap_open");
      __session.ls_conn = ldap_open (cfg->ldc_host, cfg->ldc_port);
      debug ("<== ldap_open");
#endif /* LDAP_VERSION3_API */
      if (__session.ls_conn != NULL || cfg->ldc_next == cfg)
	{
	  break;
	}
      cfg = cfg->ldc_next;
    }

  if (__session.ls_conn == NULL)
    {
      debug ("<== do_open");
      return NSS_UNAVAIL;
    }

#if defined(NETSCAPE_API_EXTENSIONS) && !defined(HAVE_LDAP_THREAD_FNS)
  if (_nss_ldap_ltf_thread_init (__session.ls_conn) != NSS_SUCCESS)
    {
      do_close ();
      debug ("<== do_open");
      return NSS_UNAVAIL;
    }
#endif /* NETSCAPE_API_EXTENSIONS */

#ifdef NETSCAPE_API_EXTENSIONS
  ldap_set_rebind_proc (__session.ls_conn, _nss_ldap_rebind, NULL);
#else
  ldap_set_rebind_proc (__session.ls_conn, _nss_ldap_rebind);
#endif /* NETSCAPE_API_EXTENSIONS */

#ifdef LDAP_VERSION3_API
  ldap_set_option (__session.ls_conn, LDAP_OPT_PROTOCOL_VERSION,
		   &cfg->ldc_version);
#else
  __session.ls_conn->ld_version = cfg->ldc_version;
#endif /* LDAP_VERSION3_API */

#ifdef LDAP_VERSION3_API
  ldap_set_option (__session.ls_conn, LDAP_OPT_DEREF,
		   &cfg->ldc_version);
#else
  __session.ls_conn->ld_deref = cfg->ldc_deref;
#endif /* LDAP_VERSION3_API */

#ifdef SSL
  /*
   * If SSL is desired, then enable it.
   */
  if (cfg->ldc_ssl_on)
    {
      if (ldapssl_install_routines (__session.ls_conn) != LDAP_SUCCESS)
	{
	  do_close ();
	  debug ("<== do_open");
	  return NSS_UNAVAIL;
	}
      if (ldap_set_option (__session.ls_conn, LDAP_OPT_SSL, LDAP_OPT_ON) !=
	  LDAP_SUCCESS)
	{
	  do_close ();
	  debug ("<== do_open");
	  return NSS_UNAVAIL;
	}
    }
#endif /* SSL */

  /*
   * If we're running as root, let us bind as a special
   * user, so we can fake shadow passwords.
   * Thanks to Doug Nazar <nazard@dragoninc.on.ca> for this
   * patch.
   */
  if (euid == 0 && cfg->ldc_rootbinddn != NULL)
    {
      if (do_bind
	  (__session.ls_conn, cfg->ldc_rootbinddn,
	   cfg->ldc_rootbindpw) != LDAP_SUCCESS)
	{
	  do_close ();
	  debug ("<== do_open");
	  return NSS_UNAVAIL;
	}
    }
  else
    {
      if (do_bind
	  (__session.ls_conn, cfg->ldc_binddn,
	   cfg->ldc_bindpw) != LDAP_SUCCESS)
	{
	  do_close ();
	  debug ("<== do_open");
	  return NSS_UNAVAIL;
	}
    }

#ifdef DISABLE_SO_KEEPALIVE
  /*
   * Disable SO_KEEPALIVE on the session's socket.
   */
  do_disable_keepalive (__session.ls_conn);
#endif /* DISABLE_SO_KEEPALIVE */

  __session.ls_config = cfg;

  debug ("<== do_open");

  return NSS_SUCCESS;
}

static int
do_bind (LDAP * ld,
	  const char * dn,
	  const char * pw)
{
  int rc;
  int msgid;
  struct timeval tv;
  LDAPMessage *result;
    
  debug("==> do_bind");
  msgid = ldap_simple_bind (ld, dn, pw);

#ifdef BIND_TIMEOUT
  tv.tv_sec = BIND_TIMEOUT;
#else
  tv.tv_sec = 30;
#endif    
  tv.tv_usec = 0;
    
  rc = ldap_result(ld, msgid, 0, &tv, &result);
  if (rc > 0)
    {
      debug("<== do_bind");
      return ldap_result2error(ld, result, 1);
    }

    /* took too long */
  if (rc == 0)
    {
      ldap_abandon(ld, msgid);
    }
    
  debug("<== do_bind");

  return -1;
}

/*
 * This function initializes an enumeration context.
 * It is called from setXXent() directly, and so can safely lock the
 * mutex. 
 *
 * It could be done from the default constructor, under Solaris, but we
 * delay it until the setXXent() function is called.
 */
ent_context_t *
_nss_ldap_ent_context_init (ent_context_t ** pctx)
{
  ent_context_t *ctx;

  debug ("==> _nss_ldap_ent_context_init");

  nss_context_lock ();

  ctx = *pctx;

  if (ctx == NULL)
    {
      ctx = (ent_context_t *) malloc (sizeof (*ctx));
      if (ctx == NULL)
	{
	  nss_context_unlock ();
	  debug ("<== _nss_ldap_ent_context_init");
	  return NULL;
	}
      *pctx = ctx;
    }
  else
    {
      if (ctx->ec_res != NULL)
	{
	  ldap_msgfree (ctx->ec_res);
	}
      if (ctx->ec_msgid > -1 && _nss_ldap_result (ctx) == NSS_SUCCESS)
	{
	  ldap_abandon (__session.ls_conn, ctx->ec_msgid);
	}
    }

  ctx->ec_res = NULL;
  ctx->ec_msgid = -1;

  LS_INIT (ctx->ec_state);

  nss_context_unlock ();

  debug ("<== _nss_ldap_ent_context_init");
  return ctx;
}

/*
 * Clears a given context; this is called from endXXent() and so we
 * can grab the lock.
 */
void
_nss_ldap_ent_context_zero (ent_context_t * ctx)
{
  debug ("==> _nss_ldap_ent_context_zero");

  nss_context_lock ();

  if (ctx == NULL)
    {
      nss_context_unlock ();
      debug ("<== _nss_ldap_ent_context_zero");
      return;
    }

  if (ctx->ec_res != NULL)
    {
      ldap_msgfree (ctx->ec_res);
      ctx->ec_res = NULL;
    }

  /*
   * Abandon the search if there were more results to fetch.
   */
  if (ctx->ec_msgid > -1 && _nss_ldap_result (ctx) == NSS_SUCCESS)
    {
      ldap_abandon (__session.ls_conn, ctx->ec_msgid);
      ctx->ec_msgid = -1;
    }

  LS_INIT (ctx->ec_state);

  nss_context_unlock ();

  debug ("<== _nss_ldap_ent_context_zero");

  return;
}

/*
 * Frees an enumeration context. This is presently
 * only used on Solaris.
 */
void
_nss_ldap_ent_context_free (ent_context_t ** ctx)
{
  debug ("==> _nss_ldap_ent_context_free");

  _nss_ldap_ent_context_zero (*ctx);
  free (*ctx);
  *ctx = NULL;

  debug ("<== _nss_ldap_ent_context_free");

  return;
}

/*
 * Do the necessary formatting to create a string filter.
 */
static NSS_STATUS
do_filter (const ldap_args_t * args, const char *filterprot,
	   const char **attrs, char *filter, size_t filterlen)
{
  char buf1[LDAP_FILT_MAXSIZ], buf2[LDAP_FILT_MAXSIZ];
  NSS_STATUS stat;

  debug ("==> do_filter");

  if (args != NULL)
    {
      switch (args->la_type)
	{
	case LA_TYPE_STRING:
	  if ((stat = _nss_ldap_escape_string(args->la_arg1.la_string, buf1, sizeof(buf1))) != NSS_SUCCESS)
	    return stat;
#ifdef HAVE_SNPRINTF
	  snprintf (filter, filterlen, filterprot, buf1);
#else
	  sprintf (filter, filterprot, buf1);
#endif
	  break;
	case LA_TYPE_NUMBER:
#ifdef HAVE_SNPRINTF
	  snprintf (filter, filterlen, filterprot, args->la_arg1.la_number);
#else
	  sprintf (filter, filterprot, args->la_arg1.la_number);
#endif
	  break;
	case LA_TYPE_STRING_AND_STRING:
	  if ((stat = _nss_ldap_escape_string(args->la_arg1.la_string, buf1, sizeof(buf1))) != NSS_SUCCESS ||
	      (stat = _nss_ldap_escape_string(args->la_arg2.la_string, buf2, sizeof(buf2)) != NSS_SUCCESS))
	    return stat;
#ifdef HAVE_SNPRINTF
	  snprintf (filter, filterlen, filterprot, buf1, buf2);
#else
	  sprintf (filter, filterprot, buf1, buf2);
#endif
	  break;
	case LA_TYPE_NUMBER_AND_STRING:
	  if ((stat = _nss_ldap_escape_string(args->la_arg2.la_string, buf1, sizeof(buf1))) != NSS_SUCCESS)
	    return stat;
#ifdef HAVE_SNPRINTF
	  snprintf (filter, filterlen, filterprot,
		    args->la_arg1.la_number, buf1);
#else
	  sprintf (filter, filterprot, args->la_arg1.la_number,
		   buf1);
#endif
	  break;
	}
    }

  debug (":== do_filter: %s\n", filter);

  debug ("<== do_filter");

  return NSS_SUCCESS;
}

/*
 * Wrapper around ldap_result() to skip over search references
 * and deal transparently with the last entry.
 */
static NSS_STATUS
do_result (ent_context_t * ctx, int all)
{
  int rc = LDAP_UNAVAILABLE;
  NSS_STATUS stat = NSS_TRYAGAIN;

  debug ("==> do_result");
  do
    {
      rc =
	ldap_result (__session.ls_conn, ctx->ec_msgid, all, NULL,
		     &ctx->ec_res);
      switch (rc)
	{
	case -1:
	case 0:
#ifdef LDAP_VERSION3_API
	  if (ldap_get_option
	      (__session.ls_conn, LDAP_OPT_ERROR_NUMBER, &rc) != LDAP_SUCCESS)
	    {
	      rc = LDAP_UNAVAILABLE;
	    }
#else
	  rc = __session.ls_conn->ld_errno;
#endif /* LDAP_VERSION3_API */
	  syslog (LOG_ERR, "nss_ldap: could not get LDAP result - %s",
		  ldap_err2string (rc));
	  stat = NSS_UNAVAIL;
	  break;
	case LDAP_RES_SEARCH_ENTRY:
	  stat = NSS_SUCCESS;
	  break;
	case LDAP_RES_SEARCH_RESULT:
	  if (all == LDAP_MSG_ALL)
	    {
	      /* we asked for the result chain, we got it. */
	      stat = NSS_SUCCESS;
	    }
	  else
	    {
#ifdef LDAP_VERSION3_API
	      int parserc;
	      /* NB: this frees ctx->ec_res */
	      parserc =
		ldap_parse_result (__session.ls_conn, ctx->ec_res, &rc, NULL,
				   NULL, NULL, NULL, 1);
	      if (parserc != LDAP_SUCCESS && parserc != LDAP_MORE_RESULTS_TO_RETURN)
		{
		  stat = NSS_UNAVAIL;
		  ldap_abandon (__session.ls_conn, ctx->ec_msgid);
		  syslog (LOG_ERR, "nss_ldap: could not get LDAP result - %s",
			  ldap_err2string (rc));
		}
	      else
		{
		  stat = NSS_NOTFOUND;
		}
#else
	      stat = NSS_NOTFOUND;
#endif /* LDAP_VERSION3_API */
	      ctx->ec_res = NULL;
	      ctx->ec_msgid = -1;
	    }
	  break;
	default:
	  stat = NSS_UNAVAIL;
	  break;
	}
    }
#ifdef LDAP_VERSION3_API
  while (rc == LDAP_RES_SEARCH_REFERENCE);
#else
  while (0);
#endif /* LDAP_VERSION3_API */

  debug ("<== do_result");

  return stat;
}

/*
 * Function to call either do_search() or do_search_s() with
 * reconnection logic.
 */
static NSS_STATUS
do_with_reconnect (const char *base, int scope,
		   const char *filter, const char **attrs, int sizelimit,
		   void *private, search_func_t search_func)
{
  int rc = LDAP_UNAVAILABLE, tries = 0, backoff = 0;
  NSS_STATUS stat = NSS_TRYAGAIN;

  debug ("==> do_with_reconnect");

  while (stat == NSS_TRYAGAIN &&
	 tries < LDAP_NSS_MAXCONNTRIES + LDAP_NSS_TRIES)
    {
      if (tries > LDAP_NSS_MAXCONNTRIES)
	{
	  if (backoff == 0)
	    backoff = LDAP_NSS_SLEEPTIME;
	  else if (backoff < LDAP_NSS_MAXSLEEPTIME)
	    backoff *= 2;

	  syslog (LOG_INFO,
		  "nss_ldap: reconnecting to LDAP server (sleeping %d seconds)...",
		  backoff);
	  (void) sleep (backoff);
	}
      else if (tries > 0)
	{
	  /* Don't sleep, reconnect immediately. */
	  syslog (LOG_INFO, "nss_ldap: reconnecting to LDAP server...");
	}

      if (do_open () != NSS_SUCCESS)
	{
	  __session.ls_conn = NULL;
	  ++tries;
	  continue;
	}

      if (search_func (base, scope, filter, attrs, sizelimit, private) ==
	  NSS_SUCCESS)
	{
	  rc = LDAP_SUCCESS;
	}
      else
	{
#ifdef LDAP_VERSION3_API
	  if (ldap_get_option
	      (__session.ls_conn, LDAP_OPT_ERROR_NUMBER, &rc) != LDAP_SUCCESS)
	    {
	      rc = LDAP_UNAVAILABLE;
	    }
#else
	  rc = __session.ls_conn->ld_errno;
#endif
	}
      switch (rc)
	{
	case LDAP_SUCCESS:	/* Huh? */
	case LDAP_SIZELIMIT_EXCEEDED:
	case LDAP_TIMELIMIT_EXCEEDED:
	  stat = NSS_SUCCESS;
	  break;
	case LDAP_SERVER_DOWN:
	case LDAP_TIMEOUT:
	case LDAP_UNAVAILABLE:
	case LDAP_BUSY:
	  do_close ();
	  stat = NSS_TRYAGAIN;
	  ++tries;
	  continue;
	  break;
	default:
	  stat = NSS_UNAVAIL;
	  break;
	}
    }

  switch (stat)
    {
    case NSS_UNAVAIL:
      syslog (LOG_ERR, "nss_ldap: could not search LDAP server - %s",
	      ldap_err2string (rc));
      break;
    case NSS_TRYAGAIN:
      syslog (LOG_ERR,
	      "nss_ldap: could not reconnect to LDAP server - %s",
	      ldap_err2string (rc));
      stat = NSS_UNAVAIL;
      break;
    case NSS_SUCCESS:
      if (tries)
	syslog (LOG_ERR,
		"nss_ldap: reconnected to LDAP server after %d attempt(s)",
		tries);
      break;
    default:
      break;
    }

  debug ("<== do_with_reconnect");
  return stat;
}

/*
 * Synchronous search function. Don't call this directly;
 * always wrap calls to this with do_with_reconnect(), or,
 * better still, use _nss_ldap_search_s().
 */
static NSS_STATUS
do_search_s (const char *base, int scope,
	     const char *filter, const char **attrs, int sizelimit,
	     LDAPMessage ** res)
{
  ent_context_t ctx;
  NSS_STATUS stat;

  debug ("==> do_search_s");

  ctx.ec_msgid = -1;
  ctx.ec_res = NULL;

  stat = do_search (base, scope, filter, attrs, sizelimit, &ctx.ec_msgid);

  if (stat == NSS_SUCCESS)
    {
      stat = do_result (&ctx, LDAP_MSG_ALL);
      if (stat == NSS_SUCCESS)
	{
	  *res = ctx.ec_res;
	}
    }

  debug ("<== do_search_s");

  return stat;
}

/*
 * Asynchronous search function. Don't call this directly;
 * always wrap calls to this with do_with_reconnect(), or,
 * better still, use _nss_ldap_search().
 */
static NSS_STATUS
do_search (const char *base, int scope,
	   const char *filter, const char **attrs, int sizelimit, int *msgid)
{
  NSS_STATUS stat;

  debug ("==> do_search");

#ifdef LDAP_VERSION3_API
  ldap_set_option (__session.ls_conn, LDAP_OPT_SIZELIMIT,
		   (void *) &sizelimit);
#else
  __session.ls_conn->ld_sizelimit = sizelimit;
#endif /* LDAP_VERSION3_API */

  *msgid = ldap_search (__session.ls_conn, base, scope, filter,
			(char **) attrs, 0);


  stat = (*msgid < 0) ? NSS_UNAVAIL : NSS_SUCCESS;

  debug ("<== do_search");

  return stat;
}

/*
 * Tries parser function "parser" on entries, calling do_result()
 * to retrieve them from the LDAP server until one parsers
 * correctly or there is an exceptional condition.
 */
static NSS_STATUS
do_parse (ent_context_t * ctx, void *result, char *buffer, size_t buflen,
	  int *errnop, parser_t parser)
{
  NSS_STATUS stat = NSS_SUCCESS;

  debug ("==> do_parse");

  /*
   * if ec_state.ls_info.ls_index is non-zero, then we don't collect another
   * entry off the LDAP chain, and instead refeed the existing result to
   * the parser. Once the parser has finished with it, it will return
   * NSS_NOTFOUND and reset the index to -1, at which point we'll retrieve
   * another entry.
   */
      if (ctx->ec_state.ls_retry == 0 && 
	  (ctx->ec_state.ls_type == LS_TYPE_KEY
	   || ctx->ec_state.ls_info.ls_index == -1))
	{
	  stat = do_result (ctx, LDAP_MSG_ONE);
	}

      if (stat == NSS_SUCCESS)
	{
	  /* we have an entry, try to parse it */

	  stat =
	    parser (__session.ls_conn, ctx->ec_res, &ctx->ec_state, result,
		    buffer, buflen);

	  ctx->ec_state.ls_retry = (stat == NSS_TRYAGAIN ? 1 : 0);
	  
          if (ctx->ec_state.ls_retry == 0 && 
	      (ctx->ec_state.ls_type == LS_TYPE_KEY
	       || ctx->ec_state.ls_info.ls_index == -1))
	    {
	      /* we don't need the result anymore, ditch it. */
	      ldap_msgfree (ctx->ec_res);
	      ctx->ec_res = NULL;
	    }
	}

  *errnop = 0;
  if (stat == NSS_TRYAGAIN)
    {
#ifdef SUN_NSS
      errno = ERANGE;
      *errnop = 1;		/* this is really erange */
#else
      *errnop = ERANGE;
#endif /* SUN_NSS */
    }

  debug ("<== do_parse");

  return stat;
}

/*
 * Read an entry from the directory, a la X.500. This is used
 * for functions that need to retrieve attributes from a DN,
 * such as the RFC2307bis group expansion function.
 */
NSS_STATUS
_nss_ldap_read (const char *dn, const char **attributes, LDAPMessage ** res)
{
  return do_with_reconnect (dn, LDAP_SCOPE_BASE, "(objectclass=*)",
			    attributes, 1,	/* sizelimit */
			    res, (search_func_t) do_search_s);
}

/*
 * Simple wrapper around ldap_get_values(). Requires that
 * session is already established.
 */
char **
_nss_ldap_get_values (LDAPMessage * e, char *attr)
{
  if (__session.ls_conn == NULL)
    {
      return NULL;
    }
  return ldap_get_values (__session.ls_conn, e, attr);
}

/*
 * Simple wrapper around ldap_get_dn(). Requires that
 * session is already established.
 */
char *
_nss_ldap_get_dn (LDAPMessage * e)
{
  if (__session.ls_conn == NULL)
    {
      return NULL;
    }
  return ldap_get_dn (__session.ls_conn, e);
}

/*
 * Simple wrapper around ldap_first_entry(). Requires that
 * session is already established.
 */
LDAPMessage *
_nss_ldap_first_entry (LDAPMessage * res)
{
  if (__session.ls_conn == NULL)
    {
      return NULL;
    }
  return ldap_first_entry (__session.ls_conn, res);
}

/*
 * Simple wrapper around ldap_next_entry(). Requires that
 * session is already established.
 */
LDAPMessage *
_nss_ldap_next_entry (LDAPMessage * res)
{
  if (__session.ls_conn == NULL)
    {
      return NULL;
    }
  return ldap_next_entry (__session.ls_conn, res);
}

/*
 * Calls ldap_result() with LDAP_MSG_ONE.
 */
NSS_STATUS
_nss_ldap_result (ent_context_t * ctx)
{
  return do_result (ctx, LDAP_MSG_ONE);
}

/*
 * The generic synchronous lookup cover function. 
 * Assumes caller holds lock.
 */
NSS_STATUS
_nss_ldap_search_s (const ldap_args_t * args,
		    const char *filterprot, const char **attrs,
		    int sizelimit, LDAPMessage ** res)
{
  char filter[LDAP_FILT_MAXSIZ];
  NSS_STATUS stat;

  debug ("==> _nss_ldap_search_s");

  stat = do_open ();
  if (stat != NSS_SUCCESS)
    {
      __session.ls_conn = NULL;
      debug ("<== _nss_ldap_search_s");
      return stat;
    }

  stat = do_filter (args, filterprot, attrs, filter, sizeof (filter));
  if (stat != NSS_SUCCESS)
    return stat;

  stat = do_with_reconnect (__session.ls_config->ldc_base,
			    __session.ls_config->ldc_scope,
			    (args == NULL) ? (char *) filterprot : filter,
			    attrs, sizelimit, res,
			    (search_func_t) do_search_s);

  debug ("<== _nss_ldap_search_s");

  return stat;
}

/*
 * The generic lookup cover function (asynchronous).
 * Assumes caller holds lock.
 */
NSS_STATUS
_nss_ldap_search (const ldap_args_t * args, const char *filterprot,
		  const char **attrs, int sizelimit, int *msgid)
{
  char filter[LDAP_FILT_MAXSIZ];
  NSS_STATUS stat;

  debug ("==> _nss_ldap_search");

  stat = do_open ();
  if (stat != NSS_SUCCESS)
    {
      __session.ls_conn = NULL;
      debug ("<== _nss_ldap_search");
      return stat;
    }

  stat = do_filter (args, filterprot, attrs, filter, sizeof (filter));
  if (stat != NSS_SUCCESS)
    return stat;

  stat = do_with_reconnect (__session.ls_config->ldc_base,
			    __session.ls_config->ldc_scope,
			    (args == NULL) ? (char *) filterprot : filter,
			    attrs, sizelimit, msgid,
			    (search_func_t) do_search);

  debug ("<== _nss_ldap_search");

  return stat;
}

/*
 * General entry point for enumeration routines.
 * This should really use the asynchronous LDAP search API to avoid
 * pulling down all the entries at once, particularly if the
 * enumeration is not completed.
 * Locks mutex.
 */
NSS_STATUS
_nss_ldap_getent (ent_context_t ** ctx,
		  void *result,
		  char *buffer,
		  size_t buflen,
		  int *errnop,
		  const char *filterprot, const char **attrs, parser_t parser)
{
  NSS_STATUS stat = NSS_SUCCESS;

  debug ("==> _nss_ldap_getent");

  if (*ctx == NULL || (*ctx)->ec_msgid == -1)
    {
      /*
       * implicitly call setent() if this is the first time
       * or there is no active search
       */
      if (_nss_ldap_ent_context_init(ctx) == NULL)
      {
        debug ("<== _nss_ldap_getent");
        return NSS_UNAVAIL;
      }
    }

  /*
   * we need to lock here as the context may not be thread-specific
   * data (under glibc, for example). Maybe we should make the lock part
   * of the context.
   */

  nss_context_lock ();

  /*
   * If ctx->ec_msgid < 0, then we haven't searched yet. Let's do it!
   */
  if ((*ctx)->ec_msgid < 0)
    {
      int msgid;

      stat =
	_nss_ldap_search (NULL, filterprot, attrs, LDAP_NO_LIMIT, &msgid);
      if (stat != NSS_SUCCESS)
	{
	  nss_context_unlock ();
	  debug ("<== _nss_ldap_getent");
	  return stat;
	}

      (*ctx)->ec_msgid = msgid;
    }


  nss_context_unlock ();

  stat = do_parse (*ctx, result, buffer, buflen, errnop, parser);

  debug ("<== _nss_ldap_getent");

  return stat;
}

/*
 * General match function.
 * Locks mutex.
 */
NSS_STATUS
_nss_ldap_getbyname (ldap_args_t * args,
		     void *result,
		     char *buffer,
		     size_t buflen,
		     int *errnop,
		     const char *filterprot,
		     const char **attrs, parser_t parser)
{
  NSS_STATUS stat = NSS_NOTFOUND;
  ent_context_t ctx;

  nss_context_lock ();

  debug ("==> _nss_ldap_getbyname");

  stat = _nss_ldap_search (args, filterprot, attrs, 1, &ctx.ec_msgid);
  if (stat != NSS_SUCCESS)
    {
      nss_context_unlock ();
      debug ("<== _nss_ldap_getbyname");
      return stat;
    }

  /*
   * we pass this along for the benefit of the services parser,
   * which uses it to figure out which protocol we really wanted.
   * we only pass the second argument along, as that's what we need
   * in services.
   */
  LS_INIT (ctx.ec_state);
  ctx.ec_state.ls_type = LS_TYPE_KEY;
  ctx.ec_state.ls_info.ls_key = args->la_arg2.la_string;

  stat = do_parse (&ctx, result, buffer, buflen, errnop, parser);

  /* is there a race condition here? */
  nss_context_unlock ();

  _nss_ldap_ent_context_zero (&ctx);

  debug ("<== _nss_ldap_getbyname");

  return stat;
}

/*
 * These functions are called from within the parser, where it is assumed
 * to be safe to use the connection and the respective message.
 */

/*
 * Assign all values, bar omitvalue (if not NULL), to *valptr.
 */
NSS_STATUS
_nss_ldap_assign_attrvals (LDAP * ld,
			   LDAPMessage * e,
			   const char *attr,
			   const char *omitvalue,
			   char ***valptr,
			   char **pbuffer,
			   size_t * pbuflen, size_t * pvalcount)
{
  char **vals;
  char **valiter;
  int valcount;
  char **p = NULL;

  register int buflen = *pbuflen;
  register char *buffer = *pbuffer;

  if (pvalcount != NULL)
    {
      *pvalcount = 0;
    }

  vals = ldap_get_values (ld, e, (char *) attr);

  valcount = (vals == NULL) ? 0 : ldap_count_values (vals);
  if (bytesleft (buffer, buflen) < (valcount + 1) * sizeof (char *))
    {
      ldap_value_free (vals);
      return NSS_TRYAGAIN;
    }

  align (buffer, buflen);
  p = *valptr = (char **) buffer;

  buffer += (valcount + 1) * sizeof (char *);
  buflen -= (valcount + 1) * sizeof (char *);

  if (valcount == 0)
    {
      *p = NULL;
      *pbuffer = buffer;
      *pbuflen = buflen;
      return NSS_SUCCESS;
    }

  valiter = vals;

  while (*valiter != NULL)
    {
      int vallen;
      char *elt = NULL;

      if (omitvalue != NULL && strcmp (*valiter, omitvalue) == 0)
	{
	  valcount--;
	}
      else
	{
	  vallen = strlen (*valiter);
	  if (buflen < (size_t) (vallen + 1))
	    {
	      ldap_value_free (vals);
	      return NSS_TRYAGAIN;
	    }

	  /* copy this value into the next block of buffer space */
	  elt = buffer;
	  buffer += vallen + 1;
	  buflen -= vallen + 1;

	  strncpy (elt, *valiter, vallen);
	  elt[vallen] = '\0';
	  *p = elt;
	  p++;
	}
      valiter++;
    }

  *p = NULL;
  *pbuffer = buffer;
  *pbuflen = buflen;

  if (pvalcount != NULL)
    {
      *pvalcount = valcount;
    }

  ldap_value_free (vals);
  return NSS_SUCCESS;
}

/* Assign a single value to *valptr. */
NSS_STATUS
_nss_ldap_assign_attrval (LDAP * ld,
			  LDAPMessage * e,
			  const char *attr,
			  char **valptr, char **buffer, size_t * buflen)
{
  char **vals;
  int vallen;

  vals = ldap_get_values (ld, e, (char *) attr);
  if (vals == NULL)
    {
      return NSS_NOTFOUND;
    }

  vallen = strlen (*vals);
  if (*buflen < (size_t) (vallen + 1))
    {
      ldap_value_free (vals);
      return NSS_TRYAGAIN;
    }

  *valptr = *buffer;

  strncpy (*valptr, *vals, vallen);
  (*valptr)[vallen] = '\0';

  *buffer += vallen + 1;
  *buflen -= vallen + 1;

  ldap_value_free (vals);

  return NSS_SUCCESS;
}


/*
 * Assign a single value to *valptr, after examining userPassword for
 * a syntactically suitable value. The behaviour here is determinable at
 * runtime from ldap.conf.
 */
NSS_STATUS
_nss_ldap_assign_passwd (LDAP * ld,
			 LDAPMessage * e,
			 const char *attr,
			 char **valptr, char **buffer, size_t * buflen)
{
  char **vals;
  char **valiter;
  char *pwd = NULL;
  int vallen;

  vals = ldap_get_values (ld, e, (char *) attr);
  if (vals != NULL)
    {
      for (valiter = vals; *valiter != NULL; valiter++)
	{
	  if (strncasecmp (*valiter,
			   "{CRYPT}", (sizeof ("{CRYPT}") - 1)) == 0)
	    {
	      pwd = *valiter;
	      break;
	    }
	}
    }

  if (pwd == NULL)
    {
      pwd = "x";
    }
  else
    {
      pwd += (sizeof ("{CRYPT}") - 1);
    }

  vallen = strlen (pwd);

  if (*buflen < (size_t) (vallen + 1))
    {
      if (vals != NULL)
	{
	  ldap_value_free (vals);
	}
      return NSS_TRYAGAIN;
    }

  *valptr = *buffer;

  strncpy (*valptr, pwd, vallen);
  (*valptr)[vallen] = '\0';

  *buffer += vallen + 1;
  *buflen -= vallen + 1;

  if (vals != NULL)
    {
      ldap_value_free (vals);
    }

  return NSS_SUCCESS;
}

NSS_STATUS _nss_ldap_oc_check (LDAP *ld,
                                LDAPMessage * e,
                                const char * oc)
{
  char **vals, **valiter;
  NSS_STATUS ret = NSS_NOTFOUND;

  vals = ldap_get_values(ld, e, "objectClass");
  if (vals != NULL)
    {
      for (valiter = vals; *valiter != NULL; valiter++)
	{
	  if (strcasecmp (*valiter, oc) == 0)
	    {
	      ret = NSS_SUCCESS;
	      break;
	    }
	}
    }

  if (vals != NULL)
    {
      ldap_value_free (vals);
    }

  return ret;
}


