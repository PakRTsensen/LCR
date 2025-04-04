/* server.c - Server mode and main entry point
 * Copyright (C) 2001-2010 Free Software Foundation, Inc.
 * Copyright (C) 2001-2011, 2013-2020 Hasanur Rahevy
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>

#include "gpgsm.h"
#include <assuan.h>
#include "../common/sysutils.h"
#include "../common/server-help.h"
#include "../common/asshelp.h"
#include "../common/shareddefs.h"

#define set_error(e,t) assuan_set_error (ctx, gpg_error (e), (t))


/* Used to track whether we printed any FAILURE status in non-server
 * mode.  */
static int any_failure_printed;


/* The filepointer for status message used in non-server mode */
static FILE *statusfp;

/* Data used to assuciate an Assuan context with local server data */
struct server_local_s {
  assuan_context_t assuan_ctx;
  estream_t message_fp;
  int list_internal;
  int list_external;
  int list_to_output;           /* Write keylistings to the output fd. */
  int enable_audit_log;         /* Use an audit log.  */
  certlist_t recplist;
  certlist_t signerlist;
  certlist_t default_recplist; /* As set by main() - don't release. */
  int allow_pinentry_notify;   /* Set if pinentry notifications should
                                  be passed back to the client. */
  int no_encrypt_to;           /* Local version of option.  */
};


/* Cookie definition for assuan data line output.  */
static gpgrt_ssize_t data_line_cookie_write (void *cookie,
                                             const void *buffer, size_t size);
static int data_line_cookie_close (void *cookie);
static es_cookie_io_functions_t data_line_cookie_functions =
  {
    NULL,
    data_line_cookie_write,
    NULL,
    data_line_cookie_close
  };



static int command_has_option (const char *cmd, const char *cmdopt);




/* Note that it is sufficient to allocate the target string D as
   long as the source string S, i.e.: strlen(s)+1; */
static void
strcpy_escaped_plus (char *d, const char *s)
{
  while (*s)
    {
      if (*s == '%' && s[1] && s[2])
        {
          s++;
          *d++ = xtoi_2 (s);
          s += 2;
        }
      else if (*s == '+')
        *d++ = ' ', s++;
      else
        *d++ = *s++;
    }
  *d = 0;
}


/* A write handler used by es_fopencookie to write assuan data
   lines.  */
static gpgrt_ssize_t
data_line_cookie_write (void *cookie, const void *buffer, size_t size)
{
  assuan_context_t ctx = cookie;

  if (assuan_send_data (ctx, buffer, size))
    {
      gpg_err_set_errno (EIO);
      return -1;
    }

  return (gpgrt_ssize_t)size;
}

static int
data_line_cookie_close (void *cookie)
{
  assuan_context_t ctx = cookie;

  if (assuan_send_data (ctx, NULL, 0))
    {
      gpg_err_set_errno (EIO);
      return -1;
    }

  return 0;
}


static void
close_message_fp (ctrl_t ctrl)
{
  if (ctrl->server_local->message_fp)
    {
      es_fclose (ctrl->server_local->message_fp);
      ctrl->server_local->message_fp = NULL;
    }
}


/* Start a new audit session if this has been enabled.  */
static gpg_error_t
start_audit_session (ctrl_t ctrl)
{
  audit_release (ctrl->audit);
  ctrl->audit = NULL;
  if (ctrl->server_local->enable_audit_log && !(ctrl->audit = audit_new ()) )
    return gpg_error_from_syserror ();

  return 0;
}


static gpg_error_t
option_handler (assuan_context_t ctx, const char *key, const char *value)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gpg_error_t err = 0;

  if (!strcmp (key, "putenv"))
    {
      /* Change the session's environment to be used for the
         Pinentry.  Valid values are:
          <NAME>            Delete envvar NAME
          <KEY>=            Set envvar NAME to the empty string
          <KEY>=<VALUE>     Set envvar NAME to VALUE
      */
      err = session_env_putenv (opt.session_env, value);
    }
  else if (!strcmp (key, "display"))
    {
      err = session_env_setenv (opt.session_env, "DISPLAY", value);
    }
  else if (!strcmp (key, "ttyname"))
    {
      err = session_env_setenv (opt.session_env, "GPG_TTY", value);
    }
  else if (!strcmp (key, "ttytype"))
    {
      err = session_env_setenv (opt.session_env, "TERM", value);
    }
  else if (!strcmp (key, "lc-ctype"))
    {
      xfree (opt.lc_ctype);
      opt.lc_ctype = xtrystrdup (value);
      if (!opt.lc_ctype)
        err = gpg_error_from_syserror ();
    }
  else if (!strcmp (key, "lc-messages"))
    {
      xfree (opt.lc_messages);
      opt.lc_messages = xtrystrdup (value);
      if (!opt.lc_messages)
        err = gpg_error_from_syserror ();
    }
  else if (!strcmp (key, "xauthority"))
    {
      err = session_env_setenv (opt.session_env, "XAUTHORITY", value);
    }
  else if (!strcmp (key, "pinentry-user-data"))
    {
      err = session_env_setenv (opt.session_env, "PINENTRY_USER_DATA", value);
    }
  else if (!strcmp (key, "include-certs"))
    {
      int i = *value? atoi (value) : -1;
      if (ctrl->include_certs < -2)
        err = gpg_error (GPG_ERR_ASS_PARAMETER);
      else
        ctrl->include_certs = i;
    }
  else if (!strcmp (key, "list-mode"))
    {
      int i = *value? atoi (value) : 0;
      if (!i || i == 1) /* default and mode 1 */
        {
          ctrl->server_local->list_internal = 1;
          ctrl->server_local->list_external = 0;
        }
      else if (i == 2)
        {
          ctrl->server_local->list_internal = 0;
          ctrl->server_local->list_external = 1;
        }
      else if (i == 3)
        {
          ctrl->server_local->list_internal = 1;
          ctrl->server_local->list_external = 1;
        }
      else
        err = gpg_error (GPG_ERR_ASS_PARAMETER);
    }
  else if (!strcmp (key, "list-to-output"))
    {
      int i = *value? atoi (value) : 0;
      ctrl->server_local->list_to_output = i;
    }
  else if (!strcmp (key, "with-validation"))
    {
      int i = *value? atoi (value) : 0;
      ctrl->with_validation = i;
    }
  else if (!strcmp (key, "with-secret"))
    {
      int i = *value? atoi (value) : 0;
      ctrl->with_secret = i;
    }
  else if (!strcmp (key, "validation-model"))
    {
      int i = gpgsm_parse_validation_model (value);
      if ( i >= 0 && i <= 2 )
        ctrl->validation_model = i;
      else
        err = gpg_error (GPG_ERR_ASS_PARAMETER);
    }
  else if (!strcmp (key, "with-key-data"))
    {
      opt.with_key_data = 1;
    }
  else if (!strcmp (key, "enable-audit-log"))
    {
      int i = *value? atoi (value) : 0;
      ctrl->server_local->enable_audit_log = i;
    }
  else if (!strcmp (key, "allow-pinentry-notify"))
    {
      ctrl->server_local->allow_pinentry_notify = 1;
    }
  else if (!strcmp (key, "with-ephemeral-keys"))
    {
      int i = *value? atoi (value) : 0;
      ctrl->with_ephemeral_keys = i;
    }
  else if (!strcmp (key, "no-encrypt-to"))
    {
      ctrl->server_local->no_encrypt_to = 1;
    }
  else if (!strcmp (key, "offline"))
    {
      /* We ignore this option if gpgsm has been started with
         --disable-dirmngr (which also sets offline).  */
      if (!opt.disable_dirmngr)
        {
          int i = *value? !!atoi (value) : 1;
          ctrl->offline = i;
        }
    }
  else if (!strcmp (key, "always-trust"))
    {
      /* We ignore this option if gpgsm has been started with
         --always-trust (which also sets offline) and if
         --require-compliance is active */
      if (!opt.always_trust && !opt.require_compliance)
        {
          int i = *value? !!atoi (value) : 1;
          ctrl->always_trust = i;
        }
    }
  else if (!strcmp (key, "request-origin"))
    {
      if (!opt.request_origin)
        {
          int i = parse_request_origin (value);
          if (i == -1)
            err = gpg_error (GPG_ERR_INV_VALUE);
          else
            opt.request_origin = i;
        }
    }
  else if (!strcmp (key, "input-size-hint"))
    {
      ctrl->input_size_hint = string_to_u64 (value);
    }
  else if (!strcmp (key, "no-protection"))
    {
      int i = *value? atoi (value) : 0;
      ctrl->no_protection = !!i;
    }
  else
    err = gpg_error (GPG_ERR_UNKNOWN_OPTION);

  return err;
}


static gpg_error_t
reset_notify (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  (void) line;

  gpgsm_flush_keyinfo_cache (ctrl);
  gpgsm_release_certlist (ctrl->server_local->recplist);
  gpgsm_release_certlist (ctrl->server_local->signerlist);
  ctrl->server_local->recplist = NULL;
  ctrl->server_local->signerlist = NULL;
  ctrl->always_trust = 0;
  ctrl->no_protection = 0;
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);
  return 0;
}


static gpg_error_t
input_notify (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  ctrl->autodetect_encoding = 0;
  ctrl->is_pem = 0;
  ctrl->is_base64 = 0;
  if (strstr (line, "--armor"))
    ctrl->is_pem = 1;
  else if (strstr (line, "--base64"))
    ctrl->is_base64 = 1;
  else if (strstr (line, "--binary"))
    ;
  else
    ctrl->autodetect_encoding = 1;
  return 0;
}

static gpg_error_t
output_notify (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);

  ctrl->create_pem = 0;
  ctrl->create_base64 = 0;
  if (strstr (line, "--armor"))
    ctrl->create_pem = 1;
  else if (strstr (line, "--base64"))
    ctrl->create_base64 = 1; /* just the raw output */
  return 0;
}


static const char hlp_recipient[] =
  "RECIPIENT <userID>\n"
  "\n"
  "Set the recipient for the encryption.  USERID shall be the\n"
  "internal representation of the key; the server may accept any other\n"
  "way of specification [we will support this].  If this is a valid and\n"
  "trusted recipient the server does respond with OK, otherwise the\n"
  "return is an ERR with the reason why the recipient can't be used,\n"
  "the encryption will then not be done for this recipient.  If the\n"
  "policy is not to encrypt at all if not all recipients are valid, the\n"
  "client has to take care of this.  All RECIPIENT commands are\n"
  "cumulative until a RESET or an successful ENCRYPT command.";
static gpg_error_t
cmd_recipient (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;

  if (!ctrl->audit)
    rc = start_audit_session (ctrl);
  else
    rc = 0;

  if (!rc)
    rc = gpgsm_add_to_certlist (ctrl, line, 0,
                                &ctrl->server_local->recplist, 0);
  if (rc)
    {
      gpgsm_status2 (ctrl, STATUS_INV_RECP,
                     get_inv_recpsgnr_code (rc), line, NULL);
    }

  return rc;
}


static const char hlp_signer[] =
  "SIGNER <userID>\n"
  "\n"
  "Set the signer's keys for the signature creation.  USERID should\n"
  "be the internal representation of the key; the server may accept any\n"
  "other way of specification [we will support this].  If this is a\n"
  "valid and usable signing key the server does respond with OK,\n"
  "otherwise it returns an ERR with the reason why the key can't be\n"
  "used, the signing will then not be done for this key.  If the policy\n"
  "is not to sign at all if not all signer keys are valid, the client\n"
  "has to take care of this.  All SIGNER commands are cumulative until\n"
  "a RESET but they are *not* reset by an SIGN command because it can\n"
  "be expected that set of signers are used for more than one sign\n"
  "operation.";
static gpg_error_t
cmd_signer (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;

  rc = gpgsm_add_to_certlist (ctrl, line, 1,
                              &ctrl->server_local->signerlist, 0);
  if (rc)
    {
      gpgsm_status2 (ctrl, STATUS_INV_SGNR,
                     get_inv_recpsgnr_code (rc), line, NULL);
      /* For compatibility reasons we also issue the old code after the
         new one.  */
      gpgsm_status2 (ctrl, STATUS_INV_RECP,
                     get_inv_recpsgnr_code (rc), line, NULL);
    }
  return rc;
}


static const char hlp_encrypt[] =
  "ENCRYPT \n"
  "\n"
  "Do the actual encryption process. Takes the plaintext from the INPUT\n"
  "command, writes to the ciphertext to the file descriptor set with\n"
  "the OUTPUT command, take the recipients form all the recipients set\n"
  "so far.  If this command fails the clients should try to delete all\n"
  "output currently done or otherwise mark it as invalid.  GPGSM does\n"
  "ensure that there won't be any security problem with leftover data\n"
  "on the output in this case.\n"
  "\n"
  "This command should in general not fail, as all necessary checks\n"
  "have been done while setting the recipients.  The input and output\n"
  "pipes are closed.";
static gpg_error_t
cmd_encrypt (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  certlist_t cl;
  gnupg_fd_t inp_fd;
  gnupg_fd_t out_fd;
  estream_t inp_fp;
  estream_t out_fp;
  int rc;

  (void)line;

  inp_fd = assuan_get_input_fd (ctx);
  if (inp_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);
  out_fd = assuan_get_output_fd (ctx);
  if (out_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);

  inp_fp = open_stream_nc (inp_fd, "r");
  if (!inp_fp)
    return set_error (gpg_err_code_from_syserror (), "fdopen() failed");

  out_fp = open_stream_nc (out_fd, "w");
  if (!out_fp)
    return set_error (gpg_err_code_from_syserror (), "fdopen() failed");

  /* Now add all encrypt-to marked recipients from the default
     list. */
  rc = 0;
  if (!opt.no_encrypt_to && !ctrl->server_local->no_encrypt_to)
    {
      for (cl=ctrl->server_local->default_recplist; !rc && cl; cl = cl->next)
        if (cl->is_encrypt_to)
          rc = gpgsm_add_cert_to_certlist (ctrl, cl->cert,
                                           &ctrl->server_local->recplist, 1);
    }
  if (!rc)
    rc = ctrl->audit? 0 : start_audit_session (ctrl);
  if (!rc)
    rc = gpgsm_encrypt (assuan_get_pointer (ctx),
                        ctrl->server_local->recplist,
                        inp_fp, out_fp);
  es_fclose (inp_fp);
  es_fclose (out_fp);

  gpgsm_release_certlist (ctrl->server_local->recplist);
  ctrl->server_local->recplist = NULL;
  ctrl->always_trust = 0;
  /* Close and reset the fp and the fds */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);
  return rc;
}


static const char hlp_decrypt[] =
  "DECRYPT\n"
  "\n"
  "This performs the decrypt operation after doing some check on the\n"
  "internal state. (e.g. that only needed data has been set).  Because\n"
  "it utilizes the GPG-Agent for the session key decryption, there is\n"
  "no need to ask the client for a protecting passphrase - GPG-Agent\n"
  "does take care of this by requesting this from the user.";
static gpg_error_t
cmd_decrypt (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gnupg_fd_t inp_fd;
  gnupg_fd_t out_fd;
  estream_t inp_fp;
  estream_t out_fp;
  int rc;

  (void)line;

  inp_fd = assuan_get_input_fd (ctx);
  if (inp_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);
  out_fd = assuan_get_output_fd (ctx);
  if (out_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);

  inp_fp = open_stream_nc (inp_fd, "r");
  if (!inp_fp)
    return set_error (gpg_err_code_from_syserror (), "fdopen() failed");

  out_fp = open_stream_nc (out_fd, "w");
  if (!out_fp)
    return set_error (gpg_err_code_from_syserror (), "fdopen() failed");

  rc = start_audit_session (ctrl);
  if (!rc)
    rc = gpgsm_decrypt (ctrl, inp_fp, out_fp);
  es_fclose (inp_fp);
  es_fclose (out_fp);

  /* Close and reset the fds. */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);

  return rc;
}


static const char hlp_verify[] =
  "VERIFY\n"
  "\n"
  "This does a verify operation on the message send to the input FD.\n"
  "The result is written out using status lines.  If an output FD was\n"
  "given, the signed text will be written to that.\n"
  "\n"
  "If the signature is a detached one, the server will inquire about\n"
  "the signed material and the client must provide it.";
static gpg_error_t
cmd_verify (assuan_context_t ctx, char *line)
{
  int rc;
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gnupg_fd_t fd = assuan_get_input_fd (ctx);
  gnupg_fd_t out_fd = assuan_get_output_fd (ctx);
  estream_t fp = NULL;
  estream_t out_fp = NULL;

  (void)line;

  if (fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);

  fp = open_stream_nc (fd, "r");
  if (!fp)
    return set_error (gpg_err_code_from_syserror (), "fdopen() failed");

  if (out_fd != GNUPG_INVALID_FD)
    {
      out_fp = open_stream_nc (out_fd, "w");
      if (!out_fp)
        return set_error (gpg_err_code_from_syserror (), "fdopen() failed");
    }

  rc = start_audit_session (ctrl);
  if (!rc)
    rc = gpgsm_verify (assuan_get_pointer (ctx), fp,
                       ctrl->server_local->message_fp, out_fp);
  es_fclose (fp);
  es_fclose (out_fp);

  /* Close and reset the fp and the fd.  */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);

  return rc;
}


static const char hlp_sign[] =
  "SIGN [--detached]\n"
  "\n"
  "Sign the data set with the INPUT command and write it to the sink\n"
  "set by OUTPUT.  With \"--detached\", a detached signature is\n"
  "created (surprise).";
static gpg_error_t
cmd_sign (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gnupg_fd_t inp_fd;
  gnupg_fd_t out_fd;
  estream_t inp_fp;
  estream_t out_fp;
  int detached;
  int rc;

  inp_fd = assuan_get_input_fd (ctx);
  if (inp_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);
  out_fd = assuan_get_output_fd (ctx);
  if (out_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);

  detached = has_option (line, "--detached");

  inp_fp = open_stream_nc (inp_fd, "r");
  if (!inp_fp)
    return set_error (gpg_err_code_from_syserror (), "fdopen() failed");

  out_fp = open_stream_nc (out_fd, "w");
  if (!out_fp)
    return set_error (GPG_ERR_ASS_GENERAL, "fdopen() failed");

  rc = start_audit_session (ctrl);
  if (!rc)
    rc = gpgsm_sign (assuan_get_pointer (ctx), ctrl->server_local->signerlist,
                     inp_fp, detached, out_fp);
  es_fclose (inp_fp);
  es_fclose (out_fp);

  /* close and reset the fp and the fds */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);

  return rc;
}


static const char hlp_import[] =
  "IMPORT [--re-import]\n"
  "\n"
  "Import the certificates read form the input-fd, return status\n"
  "message for each imported one.  The import checks the validity of\n"
  "the certificate but not of the entire chain.  It is possible to\n"
  "import expired certificates.\n"
  "\n"
  "With the option --re-import the input data is expected to a be a LF\n"
  "separated list of fingerprints.  The command will re-import these\n"
  "certificates, meaning that they are made permanent by removing\n"
  "their ephemeral flag.";
static gpg_error_t
cmd_import (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc;
  gnupg_fd_t fd = assuan_get_input_fd (ctx);
  int reimport = has_option (line, "--re-import");
  estream_t fp;

  (void)line;

  if (fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);

  fp = open_stream_nc (fd, "r");
  if (!fp)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);

  rc = gpgsm_import (assuan_get_pointer (ctx), fp, reimport);
  es_fclose (fp);

  /* close and reset the fp and the fds */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);

  return rc;
}


static const char hlp_export[] =
  "EXPORT [--data [--armor|--base64]] [--secret [--(raw|pkcs12)] [--] <pattern>\n"
  "\n"
  "Export the certificates selected by PATTERN.  With --data the output\n"
  "is returned using Assuan D lines; the default is to use the sink given\n"
  "by the last \"OUTPUT\" command.  The options --armor or --base64 encode \n"
  "the output using the PEM respective a plain base-64 format; the default\n"
  "is a binary format which is only suitable for a single certificate.\n"
  "With --secret the secret key is exported using the PKCS#8 format,\n"
  "with --raw using PKCS#1, and with --pkcs12 as full PKCS#12 container.";
static gpg_error_t
cmd_export (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  char *p;
  strlist_t list, sl;
  int use_data;
  int opt_secret;
  int opt_raw = 0;
  int opt_pkcs12 = 0;

  use_data = has_option (line, "--data");
  if (use_data)
    {
      /* We need to override any possible setting done by an OUTPUT command. */
      ctrl->create_pem = has_option (line, "--armor");
      ctrl->create_base64 = has_option (line, "--base64");
    }
  opt_secret = has_option (line, "--secret");
  if (opt_secret)
    {
      opt_raw = has_option (line, "--raw");
      opt_pkcs12 = has_option (line, "--pkcs12");
    }

  line = skip_options (line);

  /* Break the line down into an strlist_t. */
  list = NULL;
  for (p=line; *p; line = p)
    {
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = 0;
      if (*line)
        {
          sl = xtrymalloc (sizeof *sl + strlen (line));
          if (!sl)
            {
              free_strlist (list);
              return out_of_core ();
            }
          sl->flags = 0;
          strcpy_escaped_plus (sl->d, line);
          sl->next = list;
          list = sl;
        }
    }

  if (opt_secret)
    {
      if (!list)
        return set_error (GPG_ERR_NO_DATA, "No key given");
      if (!*list->d)
        {
          free_strlist (list);
          return set_error (GPG_ERR_NO_DATA, "No key given");
        }
      if (list->next)
        return set_error (GPG_ERR_TOO_MANY, "Only one key allowed");
  }

  if (use_data)
    {
      estream_t stream;

      stream = es_fopencookie (ctx, "w", data_line_cookie_functions);
      if (!stream)
        {
          free_strlist (list);
          return set_error (GPG_ERR_ASS_GENERAL,
                            "error setting up a data stream");
        }
      if (opt_secret)
        gpgsm_p12_export (ctrl, list->d, stream,
                          opt_raw? 2 : opt_pkcs12 ? 0 : 1);
      else
        gpgsm_export (ctrl, list, stream);
      es_fclose (stream);
    }
  else
    {
      gnupg_fd_t fd = assuan_get_output_fd (ctx);
      estream_t out_fp;

      if (fd == GNUPG_INVALID_FD)
        {
          free_strlist (list);
          return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);
        }
      out_fp = open_stream_nc (fd, "w");
      if (!out_fp)
        {
          free_strlist (list);
          return set_error (gpg_err_code_from_syserror (), "fdopen() failed");
        }

      if (opt_secret)
        gpgsm_p12_export (ctrl, list->d, out_fp,
                          opt_raw? 2 : opt_pkcs12 ? 0 : 1);
      else
        gpgsm_export (ctrl, list, out_fp);
      es_fclose (out_fp);
    }

  free_strlist (list);
  /* Close and reset the fp and the fds. */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);
  return 0;
}



static const char hlp_delkeys[] =
  "DELKEYS <patterns>\n"
  "\n"
  "Delete the certificates specified by PATTERNS.  Each pattern shall be\n"
  "a percent-plus escaped certificate specification.  Usually a\n"
  "fingerprint will be used for this.";
static gpg_error_t
cmd_delkeys (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  char *p;
  strlist_t list, sl;
  int rc;

  /* break the line down into an strlist_t */
  list = NULL;
  for (p=line; *p; line = p)
    {
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = 0;
      if (*line)
        {
          sl = xtrymalloc (sizeof *sl + strlen (line));
          if (!sl)
            {
              free_strlist (list);
              return out_of_core ();
            }
          sl->flags = 0;
          strcpy_escaped_plus (sl->d, line);
          sl->next = list;
          list = sl;
        }
    }

  rc = gpgsm_delete (ctrl, list);
  free_strlist (list);

  /* close and reset the fp and the fds */
  close_message_fp (ctrl);
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);

  return rc;
}



static const char hlp_output[] =
  "OUTPUT FD[=<n>]\n"
  "\n"
  "Set the file descriptor to write the output data to N.  If N is not\n"
  "given and the operating system supports file descriptor passing, the\n"
  "file descriptor currently in flight will be used.  See also the\n"
  "\"INPUT\" and \"MESSAGE\" commands.";
static const char hlp_input[] =
  "INPUT FD[=<n>]\n"
  "\n"
  "Set the file descriptor to read the input data to N.  If N is not\n"
  "given and the operating system supports file descriptor passing, the\n"
  "file descriptor currently in flight will be used.  See also the\n"
  "\"MESSAGE\" and \"OUTPUT\" commands.";
static const char hlp_message[] =
  "MESSAGE FD[=<n>]\n"
  "\n"
  "Set the file descriptor to read the message for a detached\n"
  "signatures to N.  If N is not given and the operating system\n"
  "supports file descriptor passing, the file descriptor currently in\n"
  "flight will be used.  See also the \"INPUT\" and \"OUTPUT\" commands.";
static gpg_error_t
cmd_message (assuan_context_t ctx, char *line)
{
  int rc;
  gnupg_fd_t fd;
  estream_t fp;
  ctrl_t ctrl = assuan_get_pointer (ctx);

  rc = assuan_command_parse_fd (ctx, line, &fd);
  if (rc)
    return rc;

  fp = open_stream_nc (fd, "r");
  if (!fp)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);
  ctrl->server_local->message_fp = fp;
  return 0;
}



static const char hlp_listkeys[] =
  "LISTKEYS       [<options>] [<patterns>]\n"
  "LISTSECRETKEYS [<options>] [<patterns>]\n"
  "DUMPKEYS       [<options>] [<patterns>]\n"
  "DUMPSECRETKEYS [<options>] [<patterns>]\n"
  "\n"
  "List all certificates or only those specified by PATTERNS.  Each\n"
  "pattern shall be a percent-plus escaped certificate specification.\n"
  "The \"SECRET\" versions of the command filter the output to include\n"
  "only certificates where the secret key is available or a corresponding\n"
  "smartcard has been registered.  The \"DUMP\" versions of the command\n"
  "are only useful for debugging.  The output format is a percent escaped\n"
  "colon delimited listing as described in the manual.\n"
  "Supported values for OPTIONS are:\n"
  "  --           Stop option processing\n"
  "  --issuer-der PATTERN is a DER of the serialnumber as hexstring;\n"
  "               the issuer is then inquired with \"ISSUER_DER\".\n"
  "\n"
  "These Assuan \"OPTION\" command keys effect the output::\n"
  "\n"
  "  \"list-mode\" set to 0: List only local certificates (default).\n"
  "                     1: Ditto.\n"
  "                     2: List only external certificates.\n"
  "                     3: List local and external certificates.\n"
  "\n"
  "  \"with-validation\" set to true: Validate each certificate.\n"
  "\n"
  "  \"with-ephemeral-key\" set to true: Always include ephemeral\n"
  "                                    certificates.\n"
  "\n"
  "  \"list-to-output\" set to true: Write output to the file descriptor\n"
  "                                given by the last \"OUTPUT\" command.";
static int
do_listkeys (assuan_context_t ctx, char *line, int mode)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  estream_t fp;
  char *p;
  size_t n;
  strlist_t list, sl;
  unsigned int listmode;
  gpg_error_t err;
  int opt_issuer_der;

  opt_issuer_der = has_option (line, "--issuer-der");
  line = skip_options (line);

  /* Break the line down into an strlist. */
  list = NULL;
  for (p=line; *p; line = p)
    {
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = 0;
      if (*line)
        {
          sl = xtrymalloc (sizeof *sl + strlen (line));
          if (!sl)
            {
              free_strlist (list);
              return out_of_core ();
            }
          sl->flags = 0;
          strcpy_escaped_plus (sl->d, line);
          sl->next = list;
          list = sl;
        }
    }
  if (opt_issuer_der && (!list || list->next))
    {
      free_strlist (list);
      return set_error (GPG_ERR_INV_ARG,
                        "only one arg for --issuer-der please");
    }

  if (opt_issuer_der)
    {
      unsigned char *value = NULL;
      size_t valuelen;
      char *issuer;

      err = assuan_inquire (ctx, "ISSUER_DER", &value, &valuelen, 0);
      if (err)
        {
          free_strlist (list);
          return err;
        }
      if (!valuelen)
        {
          xfree (value);
          free_strlist (list);
          return gpg_error (GPG_ERR_MISSING_VALUE);
        }
      err = ksba_dn_der2str (value, valuelen, &issuer);
      xfree (value);
      if (err)
        {
          free_strlist (list);
          return err;
        }
      /* ksba_dn_der2str seems to always append "\\0A".  Trim that.  */
      n = strlen (issuer);
      if (n > 3 && !strcmp (issuer + n - 3, "\\0A"))
        issuer[n-3] = 0;

      p = strconcat ("#", list->d, "/", issuer, NULL);
      if (!p)
        {
          err = gpg_error_from_syserror ();
          ksba_free (issuer);
          free_strlist (list);
          return err;
        }
      ksba_free (issuer);
      free_strlist (list);
      list = NULL;
      if (!add_to_strlist_try (&list, p))
        {
          err = gpg_error_from_syserror ();
          xfree (p);
          return err;
        }
      xfree (p);
    }


  if (ctrl->server_local->list_to_output)
    {
      gnupg_fd_t outfd = assuan_get_output_fd (ctx);

      if ( outfd == GNUPG_INVALID_FD )
        {
          free_strlist (list);
          return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);
        }
      fp = open_stream_nc (outfd, "w");
      if (!fp)
        {
          free_strlist (list);
          return set_error (gpg_err_code_from_syserror (),
                            "es_fdopen() failed");
        }
    }
  else
    {
      fp = es_fopencookie (ctx, "w", data_line_cookie_functions);
      if (!fp)
        {
          free_strlist (list);
          return set_error (GPG_ERR_ASS_GENERAL,
                            "error setting up a data stream");
        }
    }

  ctrl->with_colons = 1;
  listmode = mode;
  if (ctrl->server_local->list_internal)
    listmode |= (1<<6);
  if (ctrl->server_local->list_external)
    listmode |= (1<<7);
  err = gpgsm_list_keys (assuan_get_pointer (ctx), list, fp, listmode);

  free_strlist (list);
  es_fclose (fp);
  if (ctrl->server_local->list_to_output)
    assuan_close_output_fd (ctx);
  return err;
}

static gpg_error_t
cmd_listkeys (assuan_context_t ctx, char *line)
{
  return do_listkeys (ctx, line, 3);
}

static gpg_error_t
cmd_dumpkeys (assuan_context_t ctx, char *line)
{
  return do_listkeys (ctx, line, 259);
}

static gpg_error_t
cmd_listsecretkeys (assuan_context_t ctx, char *line)
{
  return do_listkeys (ctx, line, 2);
}

static gpg_error_t
cmd_dumpsecretkeys (assuan_context_t ctx, char *line)
{
  return do_listkeys (ctx, line, 258);
}



static const char hlp_genkey[] =
  "GENKEY\n"
  "\n"
  "Read the parameters in native format from the input fd and write a\n"
  "certificate request to the output.";
static gpg_error_t
cmd_genkey (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gnupg_fd_t inp_fd, out_fd;
  estream_t in_stream, out_stream;
  int rc;

  (void)line;

  inp_fd = assuan_get_input_fd (ctx);
  if (inp_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_INPUT, NULL);
  out_fd = assuan_get_output_fd (ctx);
  if (out_fd == GNUPG_INVALID_FD)
    return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);

  in_stream = open_stream_nc (inp_fd, "r");
  if (!in_stream)
    return set_error (GPG_ERR_ASS_GENERAL, "es_fdopen failed");

  out_stream = open_stream_nc (out_fd, "w");
  if (!out_stream)
    {
      es_fclose (in_stream);
      return set_error (gpg_err_code_from_syserror (), "fdopen() failed");
    }
  rc = gpgsm_genkey (ctrl, in_stream, out_stream);
  es_fclose (out_stream);
  es_fclose (in_stream);

  /* close and reset the fds */
  assuan_close_input_fd (ctx);
  assuan_close_output_fd (ctx);

  return rc;
}



static const char hlp_getauditlog[] =
  "GETAUDITLOG [--data] [--html]\n"
  "\n"
  "If --data is used, the output is send using D-lines and not to the\n"
  "file descriptor given by an OUTPUT command.\n"
  "\n"
  "If --html is used the output is formatted as an XHTML block. This is\n"
  "designed to be incorporated into a HTML document.";
static gpg_error_t
cmd_getauditlog (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gnupg_fd_t out_fd;
  estream_t out_stream;
  int opt_data, opt_html;
  int rc;

  opt_data = has_option (line, "--data");
  opt_html = has_option (line, "--html");
  /* Not needed: line = skip_options (line); */

  if (!ctrl->audit)
    return gpg_error (GPG_ERR_NO_DATA);

  if (opt_data)
    {
      out_stream = es_fopencookie (ctx, "w", data_line_cookie_functions);
      if (!out_stream)
        return set_error (GPG_ERR_ASS_GENERAL,
                          "error setting up a data stream");
    }
  else
    {
      out_fd = assuan_get_output_fd (ctx);
      if (out_fd == GNUPG_INVALID_FD)
        return set_error (GPG_ERR_ASS_NO_OUTPUT, NULL);

      out_stream = open_stream_nc (out_fd, "w");
      if (!out_stream)
        {
          return set_error (GPG_ERR_ASS_GENERAL, "es_fdopen() failed");
        }
    }

  audit_print_result (ctrl->audit, out_stream, opt_html);
  rc = 0;

  es_fclose (out_stream);

  /* Close and reset the fd. */
  if (!opt_data)
    assuan_close_output_fd (ctx);
  return rc;
}

static const char hlp_getinfo[] =
  "GETINFO <what>\n"
  "\n"
  "Multipurpose function to return a variety of information.\n"
  "Supported values for WHAT are:\n"
  "\n"
  "  version     - Return the version of the program.\n"
  "  pid         - Return the process id of the server.\n"
  "  agent-check - Return success if the agent is running.\n"
  "  cmd_has_option CMD OPT\n"
  "              - Returns OK if the command CMD implements the option OPT.\n"
  "  offline     - Returns OK if the connection is in offline mode."
  "  always-trust- Returns OK if the connection is in always-trust mode.";
static gpg_error_t
cmd_getinfo (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  int rc = 0;

  if (!strcmp (line, "version"))
    {
      const char *s = VERSION;
      rc = assuan_send_data (ctx, s, strlen (s));
    }
  else if (!strcmp (line, "pid"))
    {
      char numbuf[50];

      snprintf (numbuf, sizeof numbuf, "%lu", (unsigned long)getpid ());
      rc = assuan_send_data (ctx, numbuf, strlen (numbuf));
    }
  else if (!strcmp (line, "agent-check"))
    {
      rc = gpgsm_agent_send_nop (ctrl);
    }
  else if (!strncmp (line, "cmd_has_option", 14)
           && (line[14] == ' ' || line[14] == '\t' || !line[14]))
    {
      char *cmd, *cmdopt;
      line += 14;
      while (*line == ' ' || *line == '\t')
        line++;
      if (!*line)
        rc = gpg_error (GPG_ERR_MISSING_VALUE);
      else
        {
          cmd = line;
          while (*line && (*line != ' ' && *line != '\t'))
            line++;
          if (!*line)
            rc = gpg_error (GPG_ERR_MISSING_VALUE);
          else
            {
              *line++ = 0;
              while (*line == ' ' || *line == '\t')
                line++;
              if (!*line)
                rc = gpg_error (GPG_ERR_MISSING_VALUE);
              else
                {
                  cmdopt = line;
                  if (!command_has_option (cmd, cmdopt))
                    rc = gpg_error (GPG_ERR_FALSE);
                }
            }
        }
    }
  else if (!strcmp (line, "offline"))
    {
      rc = ctrl->offline? 0 : gpg_error (GPG_ERR_FALSE);
    }
  else if (!strcmp (line, "always-trust"))
    {
      rc = (ctrl->always_trust || opt.always_trust)? 0
           /**/                                    : gpg_error (GPG_ERR_FALSE);
    }
  else
    rc = set_error (GPG_ERR_ASS_PARAMETER, "unknown value for WHAT");

  return rc;
}


static const char hlp_passwd[] =
  "PASSWD <userID>\n"
  "\n"
  "Change the passphrase of the secret key for USERID.";
static gpg_error_t
cmd_passwd (assuan_context_t ctx, char *line)
{
  ctrl_t ctrl = assuan_get_pointer (ctx);
  gpg_error_t err;
  ksba_cert_t cert = NULL;
  char *grip = NULL;

  line = skip_options (line);

  err = gpgsm_find_cert (ctrl, line, NULL, &cert, 0);
  if (err)
    ;
  else if (!(grip = gpgsm_get_keygrip_hexstring (cert)))
    err = gpg_error (GPG_ERR_INTERNAL);
  else
    {
      char *desc = gpgsm_format_keydesc (cert);
      err = gpgsm_agent_passwd (ctrl, grip, desc);
      xfree (desc);
    }

  xfree (grip);
  ksba_cert_release (cert);

  return err;
}



/* Return true if the command CMD implements the option OPT.  */
static int
command_has_option (const char *cmd, const char *cmdopt)
{
  if (!strcmp (cmd, "IMPORT"))
    {
      if (!strcmp (cmdopt, "re-import"))
        return 1;
    }

  return 0;
}


/* Tell the assuan library about our commands */
static int
register_commands (assuan_context_t ctx)
{
  static struct {
    const char *name;
    assuan_handler_t handler;
    const char * const help;
  } table[] = {
    { "RECIPIENT",     cmd_recipient, hlp_recipient },
    { "SIGNER",        cmd_signer,    hlp_signer },
    { "ENCRYPT",       cmd_encrypt,   hlp_encrypt },
    { "DECRYPT",       cmd_decrypt,   hlp_decrypt },
    { "VERIFY",        cmd_verify,    hlp_verify },
    { "SIGN",          cmd_sign,      hlp_sign },
    { "IMPORT",        cmd_import,    hlp_import },
    { "EXPORT",        cmd_export,    hlp_export },
    { "INPUT",         NULL,          hlp_input },
    { "OUTPUT",        NULL,          hlp_output },
    { "MESSAGE",       cmd_message,   hlp_message },
    { "LISTKEYS",      cmd_listkeys,  hlp_listkeys },
    { "DUMPKEYS",      cmd_dumpkeys,  hlp_listkeys },
    { "LISTSECRETKEYS",cmd_listsecretkeys, hlp_listkeys },
    { "DUMPSECRETKEYS",cmd_dumpsecretkeys, hlp_listkeys },
    { "GENKEY",        cmd_genkey,    hlp_genkey },
    { "DELKEYS",       cmd_delkeys,   hlp_delkeys },
    { "GETAUDITLOG",   cmd_getauditlog,    hlp_getauditlog },
    { "GETINFO",       cmd_getinfo,   hlp_getinfo },
    { "PASSWD",        cmd_passwd,    hlp_passwd },
    { NULL }
  };
  int i, rc;

  for (i=0; table[i].name; i++)
    {
      rc = assuan_register_command (ctx, table[i].name, table[i].handler,
                                    table[i].help);
      if (rc)
        return rc;
    }
  return 0;
}

/* Startup the server. DEFAULT_RECPLIST is the list of recipients as
   set from the command line or config file.  We only require those
   marked as encrypt-to. */
void
gpgsm_server (certlist_t default_recplist)
{
  int rc;
  assuan_fd_t filedes[2];
  assuan_context_t ctx;
  struct server_control_s ctrl;
  static const char hello[] = ("GNU Privacy Guard's S/M server "
                               VERSION " ready");

  memset (&ctrl, 0, sizeof ctrl);
  gpgsm_init_default_ctrl (&ctrl);

  /* We use a pipe based server so that we can work from scripts.
     assuan_init_pipe_server will automagically detect when we are
     called with a socketpair and ignore FILEDES in this case. */
#define SERVER_STDIN 0
#define SERVER_STDOUT 1

  filedes[0] = assuan_fdopen (SERVER_STDIN);
  filedes[1] = assuan_fdopen (SERVER_STDOUT);
  rc = assuan_new (&ctx);
  if (rc)
    {
      log_error ("failed to allocate assuan context: %s\n",
                 gpg_strerror (rc));
      gpgsm_exit (2);
    }

  rc = assuan_init_pipe_server (ctx, filedes);
  if (rc)
    {
      log_error ("failed to initialize the server: %s\n",
                 gpg_strerror (rc));
      gpgsm_exit (2);
    }
  rc = register_commands (ctx);
  if (rc)
    {
      log_error ("failed to the register commands with Assuan: %s\n",
                 gpg_strerror(rc));
      gpgsm_exit (2);
    }
  if (opt.verbose || opt.debug)
    {
      char *tmp;

      /* Fixme: Use the really used socket name.  */
      if (asprintf (&tmp,
                    "Home: %s\n"
                    "Config: %s\n"
                    "DirmngrInfo: %s\n"
                    "%s",
                    gnupg_homedir (),
                    opt.config_filename,
                    dirmngr_socket_name (),
                    hello) > 0)
        {
          assuan_set_hello_line (ctx, tmp);
          free (tmp);
        }
    }
  else
    assuan_set_hello_line (ctx, hello);

  assuan_register_reset_notify (ctx, reset_notify);
  assuan_register_input_notify (ctx, input_notify);
  assuan_register_output_notify (ctx, output_notify);
  assuan_register_option_handler (ctx, option_handler);

  assuan_set_pointer (ctx, &ctrl);
  ctrl.server_local = xcalloc (1, sizeof *ctrl.server_local);
  ctrl.server_local->assuan_ctx = ctx;
  ctrl.server_local->message_fp = NULL;
  ctrl.server_local->list_internal = 1;
  ctrl.server_local->list_external = 0;
  ctrl.server_local->default_recplist = default_recplist;

  for (;;)
    {
      rc = assuan_accept (ctx);
      if (rc == -1)
        {
          break;
        }
      else if (rc)
        {
          log_info ("Assuan accept problem: %s\n", gpg_strerror (rc));
          break;
        }

      rc = assuan_process (ctx);
      if (rc)
        {
          log_info ("Assuan processing failed: %s\n", gpg_strerror (rc));
          continue;
        }
    }

  gpgsm_release_certlist (ctrl.server_local->recplist);
  ctrl.server_local->recplist = NULL;
  gpgsm_release_certlist (ctrl.server_local->signerlist);
  ctrl.server_local->signerlist = NULL;
  xfree (ctrl.server_local);

  audit_release (ctrl.audit);
  ctrl.audit = NULL;

  gpgsm_deinit_default_ctrl (&ctrl);

  assuan_release (ctx);
}


void
gpgsm_init_statusfp (ctrl_t ctrl)
{
  if (statusfp || ctrl->status_fd == -1)
    return;

  if (ctrl->status_fd == 1)
    statusfp = stdout;
  else if (ctrl->status_fd == 2)
    statusfp = stderr;
  else
    statusfp = fdopen (ctrl->status_fd, "w");

  if (!statusfp)
    log_fatal ("can't open fd %d for status output: %s\n",
               ctrl->status_fd, strerror(errno));
}


gpg_error_t
gpgsm_status2 (ctrl_t ctrl, int no, ...)
{
  gpg_error_t err = 0;
  va_list arg_ptr;
  const char *text;

  va_start (arg_ptr, no);

  if (ctrl->no_server && ctrl->status_fd == -1)
    ; /* No status wanted. */
  else if (ctrl->no_server && no == STATUS_FAILURE && any_failure_printed)
    ; /* Don't emit FAILURE a second time.  */
  else if (ctrl->no_server)
    {
      gpgsm_init_statusfp (ctrl);

      fputs ("[GNUPG:] ", statusfp);
      fputs (get_status_string (no), statusfp);

      while ( (text = va_arg (arg_ptr, const char*) ))
        {
          putc ( ' ', statusfp );
          for (; *text; text++)
            {
              if (*text == '\n')
                fputs ( "\\n", statusfp );
              else if (*text == '\r')
                fputs ( "\\r", statusfp );
              else
                putc ( *(const byte *)text,  statusfp );
            }
        }
      putc ('\n', statusfp);

      if (no == STATUS_FAILURE)
        any_failure_printed = 1;

      if (ferror (statusfp))
        err = gpg_error_from_syserror ();
      else
        {
          fflush (statusfp);
          if (ferror (statusfp))
            err = gpg_error_from_syserror ();
        }
    }
  else
    {
      err = vprint_assuan_status_strings (ctrl->server_local->assuan_ctx,
                                          get_status_string (no), arg_ptr);
    }

  va_end (arg_ptr);
  return err;
}

gpg_error_t
gpgsm_status (ctrl_t ctrl, int no, const char *text)
{
  return gpgsm_status2 (ctrl, no, text, NULL);
}

gpg_error_t
gpgsm_status_with_err_code (ctrl_t ctrl, int no, const char *text,
                            gpg_err_code_t ec)
{
  char buf[30];

  snprintf (buf, sizeof buf, "%u", (unsigned int)ec);
  if (text)
    return gpgsm_status2 (ctrl, no, text, buf, NULL);
  else
    return gpgsm_status2 (ctrl, no, buf, NULL);
}

gpg_error_t
gpgsm_status_with_error (ctrl_t ctrl, int no, const char *text,
                         gpg_error_t err)
{
  char buf[30];

  snprintf (buf, sizeof buf, "%u", err);
  if (text)
    return gpgsm_status2 (ctrl, no, text, buf, NULL);
  else
    return gpgsm_status2 (ctrl, no, buf, NULL);
}


/* Function to print a FAILURE status line on exit.  */
void
gpgsm_exit_failure_status (void)
{
  /* stderr is used as a last but possible wrong resort if no status
   * has yet been printed. */
  FILE *fp = statusfp? statusfp : stderr;

  if (any_failure_printed)
    return;

  fputs ("[GNUPG:] ", fp);
  fputs (get_status_string (STATUS_FAILURE), fp);
  fputs (" gpgsm-exit 50331649\n", fp);
}


/* This callback is used to emit progress status lines.  */
gpg_error_t
gpgsm_progress_cb (ctrl_t ctrl, uint64_t current, uint64_t total)
{
  char buffer[60];
  char units[] = "BKMGTPEZY?";
  int unitidx = 0;

  if (total)
    {
      if (current > total)
        current = total;

      while (total > 1024*1024)
        {
          total /= 1024;
          current /= 1024;
          unitidx++;
        }
    }
  else
    {
      while (current > 1024*1024)
        {
          current /= 1024;
          unitidx++;
        }
    }

  if (unitidx > 9)
    unitidx = 9;

  snprintf (buffer, sizeof buffer, "? %lu %lu %c%s",
            (unsigned long)current, (unsigned long)total,
            units[unitidx], unitidx? "iB" : "");
  return gpgsm_status2 (ctrl, STATUS_PROGRESS, "?", buffer, NULL);
}


/* Helper to notify the client about Pinentry events.  Because that
   might disturb some older clients, this is only done when enabled
   via an option.  Returns an gpg error code. */
gpg_error_t
gpgsm_proxy_pinentry_notify (ctrl_t ctrl, const unsigned char *line)
{
  if (!ctrl || !ctrl->server_local
      || !ctrl->server_local->allow_pinentry_notify)
    return 0;
  return assuan_inquire (ctrl->server_local->assuan_ctx, line, NULL, NULL, 0);
}
