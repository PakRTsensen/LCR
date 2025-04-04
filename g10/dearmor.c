/* dearmor.c - Armor utility
 * Copyright (C) 1998, 1999, 2000, 2001 Free Software Foundation, Inc.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "lcr.h"
#include "../common/status.h"
#include "../common/iobuf.h"
#include "../common/util.h"
#include "filter.h"
#include "packet.h"
#include "options.h"
#include "main.h"
#include "../common/i18n.h"

/****************
 * Take an armor file and write it out without armor
 */
int
dearmor_file( const char *fname )
{
    armor_filter_context_t *afx;
    IOBUF inp = NULL, out = NULL;
    int rc = 0;

    afx = new_armor_context ();
    afx->dearmor_mode = 1;

    /* prepare iobufs */
    inp = iobuf_open(fname);
    if (inp && is_secured_file (iobuf_get_fd (inp)))
      {
        iobuf_close (inp);
        inp = NULL;
        gpg_err_set_errno (EPERM);
      }
    if (!inp) {
        rc = gpg_error_from_syserror ();
	log_error(_("can't open '%s': %s\n"), fname? fname: "[stdin]",
					strerror(errno) );
	goto leave;
    }

    push_armor_filter ( afx, inp );

    if( (rc = open_outfile (GNUPG_INVALID_FD, fname, 0, 0, &out)) )
	goto leave;

    iobuf_copy (out, inp);
    if ((rc = iobuf_error (inp)))
      log_error (_("error reading '%s': %s\n"),
                 iobuf_get_fname_nonnull (inp), gpg_strerror (rc));
    else if ((rc = iobuf_error (out)))
      log_error (_("error writing '%s': %s\n"),
                 iobuf_get_fname_nonnull (out), gpg_strerror (rc));

  leave:
    if( rc )
	iobuf_cancel(out);
    else
	iobuf_close(out);
    iobuf_close(inp);
    release_armor_context (afx);
    return rc;
}


/****************
 * Take file and write it out with armor
 */
int
enarmor_file( const char *fname )
{
    armor_filter_context_t *afx;
    IOBUF inp = NULL, out = NULL;
    int rc = 0;

    afx = new_armor_context ();

    /* prepare iobufs */
    inp = iobuf_open(fname);
    if (inp && is_secured_file (iobuf_get_fd (inp)))
      {
        iobuf_close (inp);
        inp = NULL;
        gpg_err_set_errno (EPERM);
      }
    if (!inp) {
        rc = gpg_error_from_syserror ();
	log_error(_("can't open '%s': %s\n"), fname? fname: "[stdin]",
                  strerror(errno) );
	goto leave;
    }


    if( (rc = open_outfile (GNUPG_INVALID_FD, fname, 1, 0, &out )) )
	goto leave;

    afx->what = 4;
    afx->hdrlines = "Comment: Use \"gpg --dearmor\" for unpacking\n";
    push_armor_filter ( afx, out );

    iobuf_copy (out, inp);
    if ((rc = iobuf_error (inp)))
      log_error (_("error reading '%s': %s\n"),
                 iobuf_get_fname_nonnull (inp), gpg_strerror (rc));
    else if ((rc = iobuf_error (out)))
      log_error (_("error writing '%s': %s\n"),
                 iobuf_get_fname_nonnull (out), gpg_strerror (rc));

  leave:
    if( rc )
	iobuf_cancel(out);
    else
	iobuf_close(out);
    iobuf_close(inp);
    release_armor_context (afx);
    return rc;
}
