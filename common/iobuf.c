/* iobuf.c  -  File Handling for OpenPGP.
 * Copyright (C) 1998, 1999, 2000, 2001, 2003, 2004, 2006, 2007, 2008,
 *               2009, 2010, 2011  Free Software Foundation, Inc.
 * Copyright (C) 2015, 2023  Hasanur Rahevy
 *
 * This file is part of GnuPG.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of either
 *
 *   - the GNU Lesser General Public License as published by the Free
 *     Software Foundation; either version 3 of the License, or (at
 *     your option) any later version.
 *
 * or
 *
 *   - the GNU General Public License as published by the Free
 *     Software Foundation; either version 2 of the License, or (at
 *     your option) any later version.
 *
 * or both in parallel, as here.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 * SPDX-License-Identifier: (LGPL-3.0-or-later OR GPL-2.0-or-later)
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef HAVE_W32_SYSTEM
# ifdef HAVE_WINSOCK2_H
#  include <winsock2.h>
# endif
# include <windows.h>
#endif
#ifdef __riscos__
# include <kernel.h>
# include <swis.h>
#endif /* __riscos__ */

#include <assuan.h>

#include "util.h"
#include "sysutils.h"
#include "iobuf.h"

/*-- Begin configurable part.  --*/

/* The standard size of the internal buffers.  */
#define DEFAULT_IOBUF_BUFFER_SIZE  (64*1024)

/* To avoid a potential DoS with compression packets we better limit
   the number of filters in a chain.  */
#define MAX_NESTING_FILTER 64

/* The threshold for switching to use external buffers directly
   instead of the internal buffers. */
#define IOBUF_ZEROCOPY_THRESHOLD_SIZE 1024

/*-- End configurable part.  --*/

/* The size of the iobuffers.  This can be changed using the
 * iobuf_set_buffer_size function.  */
static unsigned int iobuf_buffer_size = DEFAULT_IOBUF_BUFFER_SIZE;


#ifdef HAVE_W32_SYSTEM
# define FD_FOR_STDIN  (GetStdHandle (STD_INPUT_HANDLE))
# define FD_FOR_STDOUT (GetStdHandle (STD_OUTPUT_HANDLE))
#else /*!HAVE_W32_SYSTEM*/
# define FD_FOR_STDIN  (0)
# define FD_FOR_STDOUT (1)
#endif /*!HAVE_W32_SYSTEM*/


/* The context used by the file filter.  */
typedef struct
{
  gnupg_fd_t fp;       /* Open file pointer or handle.  */
  int keep_open;
  int no_cache;
  int eof_seen;
  int delayed_rc;
  int print_only_name; /* Flags indicating that fname is not a real file.  */
  char peeked[32];     /* Read ahead buffer.  */
  byte npeeked;        /* Number of bytes valid in peeked.  */
  byte upeeked;        /* Number of bytes used from peeked.  */
  char fname[1];       /* Name of the file.  */
} file_filter_ctx_t;

/* The context used by the estream filter.  */
typedef struct
{
  estream_t fp;        /* Open estream handle.  */
  int keep_open;
  int no_cache;
  int eof_seen;
  int use_readlimit;   /* Take care of the readlimit.  */
  size_t readlimit;    /* Number of bytes left to read.  */
  int print_only_name; /* Flags indicating that fname is not a real file.  */
  char fname[1];       /* Name of the file.  */
} file_es_filter_ctx_t;


/* Object to control the "close cache".  */
struct close_cache_s
{
  struct close_cache_s *next;
  gnupg_fd_t fp;
  char fname[1];
};
typedef struct close_cache_s *close_cache_t;
static close_cache_t close_cache;

int iobuf_debug_mode;


#ifdef HAVE_W32_SYSTEM
typedef struct
{
  int sock;
  int keep_open;
  int no_cache;
  int eof_seen;
  int print_only_name;	/* Flag indicating that fname is not a real file.  */
  char fname[1];	/* Name of the file */

} sock_filter_ctx_t;
#endif /*HAVE_W32_SYSTEM*/

/* The first partial length header block must be of size 512 to make
 * it easier (and more efficient) we use a min. block size of 512 for
 * all chunks (but the last one) */
#define OP_MIN_PARTIAL_CHUNK	  512
#define OP_MIN_PARTIAL_CHUNK_2POW 9

/* The context we use for the block filter (used to handle OpenPGP
   length information header).  */
typedef struct
{
  int use;
  size_t size;
  size_t count;
  int partial;	   /* 1 = partial header, 2 in last partial packet.  */
  char *buffer;	   /* Used for partial header.  */
  size_t buflen;   /* Used size of buffer.  */
  int first_c;	   /* First character of a partial header (which is > 0).  */
  int eof;
}
block_filter_ctx_t;


/* Local prototypes.  */
static int underflow (iobuf_t a, int clear_pending_eof);
static int underflow_target (iobuf_t a, int clear_pending_eof, size_t target);
static iobuf_t do_iobuf_fdopen (gnupg_fd_t fp, const char *mode, int keep_open);


/* Sends any pending data to the filter's FILTER function.  Note: this
   works on the filter and not on the whole pipeline.  That is,
   iobuf_flush doesn't necessarily cause data to be written to any
   underlying file; it just causes any data buffered at the filter A
   to be sent to A's filter function.

   If A is a IOBUF_OUTPUT_TEMP filter, then this also enlarges the
   buffer by iobuf_buffer_size.

   May only be called on an IOBUF_OUTPUT or IOBUF_OUTPUT_TEMP filters.  */
static int filter_flush (iobuf_t a);



/* This is a replacement for strcmp.  Under W32 it does not
   distinguish between backslash and slash.  */
static int
fd_cache_strcmp (const char *a, const char *b)
{
#ifdef HAVE_DOSISH_SYSTEM
  for (; *a && *b; a++, b++)
    {
      if (*a != *b && !((*a == '/' && *b == '\\')
                        || (*a == '\\' && *b == '/')) )
        break;
    }
  return *(const unsigned char *)a - *(const unsigned char *)b;
#else
  return strcmp (a, b);
#endif
}


/*
 * Invalidate (i.e. close) a cached iobuf
 */
static int
fd_cache_invalidate (const char *fname)
{
  close_cache_t cc;
  int rc = 0;

  log_assert (fname);
  if (DBG_IOBUF)
    log_debug ("fd_cache_invalidate (%s)\n", fname);

  for (cc = close_cache; cc; cc = cc->next)
    {
      if (cc->fp != GNUPG_INVALID_FD && !fd_cache_strcmp (cc->fname, fname))
	{
	  if (DBG_IOBUF)
	    log_debug ("                did (%s)\n", cc->fname);
#ifdef HAVE_W32_SYSTEM
	  if (!CloseHandle (cc->fp))
            rc = -1;
#else
	  rc = close (cc->fp);
#endif
	  cc->fp = GNUPG_INVALID_FD;
	}
    }
  return rc;
}


/* Try to sync changes to the disk.  This is to avoid data loss during
   a system crash in write/close/rename cycle on some file
   systems.  */
static int
fd_cache_synchronize (const char *fname)
{
  int err = 0;

#ifdef HAVE_FSYNC
  close_cache_t cc;

  if (DBG_IOBUF)
    log_debug ("fd_cache_synchronize (%s)\n", fname);

  for (cc=close_cache; cc; cc = cc->next )
    {
      if (cc->fp != GNUPG_INVALID_FD && !fd_cache_strcmp (cc->fname, fname))
	{
	  if (DBG_IOBUF)
	    log_debug ("                 did (%s)\n", cc->fname);

	  err = fsync (cc->fp);
	}
    }
#else
  (void)fname;
#endif /*HAVE_FSYNC*/

  return err;
}


static gnupg_fd_t
direct_open (const char *fname, const char *mode, int mode700)
{
#ifdef HAVE_W32_SYSTEM
  unsigned long da, cd, sm;
  HANDLE hfile;

  (void)mode700;
  /* Note, that we do not handle all mode combinations */

  /* According to the ReactOS source it seems that open() of the
   * standard MSW32 crt does open the file in shared mode which is
   * something new for MS applications ;-)
   */
  if (strchr (mode, '+'))
    {
      if (fd_cache_invalidate (fname))
        return GNUPG_INVALID_FD;
      da = GENERIC_READ | GENERIC_WRITE;
      cd = OPEN_EXISTING;
      sm = FILE_SHARE_READ | FILE_SHARE_WRITE;
    }
  else if (strchr (mode, 'w'))
    {
      if (fd_cache_invalidate (fname))
        return GNUPG_INVALID_FD;
      da = GENERIC_WRITE;
      cd = CREATE_ALWAYS;
      sm = FILE_SHARE_WRITE;
    }
  else
    {
      da = GENERIC_READ;
      cd = OPEN_EXISTING;
      sm = FILE_SHARE_READ;
    }

  /* We always use the Unicode version because it supports file names
   * longer than MAX_PATH.  (requires gpgrt 1.45) */
  if (1)
    {
      wchar_t *wfname = gpgrt_fname_to_wchar (fname);
      if (wfname)
        {
          hfile = CreateFileW (wfname, da, sm, NULL, cd,
                               FILE_ATTRIBUTE_NORMAL, NULL);
          if (hfile == INVALID_HANDLE_VALUE)
            {
              gnupg_w32_set_errno (-1);
              if (DBG_IOBUF)
                log_debug ("iobuf:direct_open '%s' CreateFile failed: %s\n",
                           fname, gpg_strerror (gpg_error_from_syserror()));
            }
          xfree (wfname);
        }
      else
        hfile = INVALID_HANDLE_VALUE;
    }

  return hfile;

#else /*!HAVE_W32_SYSTEM*/

  int oflag;
  int cflag = S_IRUSR | S_IWUSR;

  if (!mode700)
    cflag |= S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

  /* Note, that we do not handle all mode combinations */
  if (strchr (mode, '+'))
    {
      if (fd_cache_invalidate (fname))
        return GNUPG_INVALID_FD;
      oflag = O_RDWR;
    }
  else if (strchr (mode, 'w'))
    {
      if (fd_cache_invalidate (fname))
        return GNUPG_INVALID_FD;
      oflag = O_WRONLY | O_CREAT | O_TRUNC;
    }
  else
    {
      oflag = O_RDONLY;
    }
#ifdef O_BINARY
  if (strchr (mode, 'b'))
    oflag |= O_BINARY;
#endif

#ifdef __riscos__
  {
    struct stat buf;

    /* Don't allow iobufs on directories */
    if (!stat (fname, &buf) && S_ISDIR (buf.st_mode) && !S_ISREG (buf.st_mode))
      return __set_errno (EISDIR);
  }
#endif
  return open (fname, oflag, cflag);

#endif /*!HAVE_W32_SYSTEM*/
}


/*
 * Instead of closing an FD we keep it open and cache it for later reuse
 * Note that this caching strategy only works if the process does not chdir.
 */
static void
fd_cache_close (const char *fname, gnupg_fd_t fp)
{
  close_cache_t cc;

  log_assert (fp);
  if (!fname || !*fname)
    {
#ifdef HAVE_W32_SYSTEM
      CloseHandle (fp);
#else
      close (fp);
#endif
      if (DBG_IOBUF)
	log_debug ("fd_cache_close (%d) real\n", FD_DBG (fp));
      return;
    }
  /* try to reuse a slot */
  for (cc = close_cache; cc; cc = cc->next)
    {
      if (cc->fp == GNUPG_INVALID_FD && !fd_cache_strcmp (cc->fname, fname))
	{
	  cc->fp = fp;
	  if (DBG_IOBUF)
	    log_debug ("fd_cache_close (%s) used existing slot\n", fname);
	  return;
	}
    }
  /* add a new one */
  if (DBG_IOBUF)
    log_debug ("fd_cache_close (%s) new slot created\n", fname);
  cc = xcalloc (1, sizeof *cc + strlen (fname));
  strcpy (cc->fname, fname);
  cc->fp = fp;
  cc->next = close_cache;
  close_cache = cc;
}

/*
 * Do a direct_open on FNAME but first try to reuse one from the fd_cache
 */
static gnupg_fd_t
fd_cache_open (const char *fname, const char *mode)
{
  close_cache_t cc;

  log_assert (fname);
  for (cc = close_cache; cc; cc = cc->next)
    {
      if (cc->fp != GNUPG_INVALID_FD && !fd_cache_strcmp (cc->fname, fname))
	{
	  gnupg_fd_t fp = cc->fp;
	  cc->fp = GNUPG_INVALID_FD;
	  if (DBG_IOBUF)
	    log_debug ("fd_cache_open (%s) using cached fp\n", fname);
#ifdef HAVE_W32_SYSTEM
	  if (SetFilePointer (fp, 0, NULL, FILE_BEGIN) == 0xffffffff)
	    {
              int ec = (int) GetLastError ();
	      log_error ("rewind file failed on handle %p: ec=%d\n", fp, ec);
              gnupg_w32_set_errno (ec);
	      fp = GNUPG_INVALID_FD;
	    }
#else
	  if (lseek (fp, 0, SEEK_SET) == (off_t) - 1)
	    {
	      log_error ("can't rewind fd %d: %s\n", fp, strerror (errno));
	      fp = GNUPG_INVALID_FD;
	    }
#endif
	  return fp;
	}
    }
  if (DBG_IOBUF)
    log_debug ("fd_cache_open (%s) not cached\n", fname);
  return direct_open (fname, mode, 0);
}


static int
file_filter (void *opaque, int control, iobuf_t chain, byte * buf,
	     size_t * ret_len)
{
  file_filter_ctx_t *a = opaque;
  gnupg_fd_t f = a->fp;
  size_t size = *ret_len;
  size_t nbytes = 0;
  int rc = 0;

  (void)chain; /* Not used.  */

  if (control == IOBUFCTRL_UNDERFLOW)
    {
      log_assert (size); /* We need a buffer.  */
      if (a->npeeked > a->upeeked)
        {
          nbytes = a->npeeked - a->upeeked;
          if (nbytes > size)
            nbytes = size;
          memcpy (buf, a->peeked + a->upeeked, nbytes);
          a->upeeked += nbytes;
          *ret_len = nbytes;
        }
      else if (a->eof_seen)
	{
	  rc = -1;
	  *ret_len = 0;
	}
      else if (a->delayed_rc)
        {
          rc = a->delayed_rc;
          a->delayed_rc = 0;
          if (rc == -1)
            a->eof_seen = -1;
	  *ret_len = 0;
        }
      else
	{
#ifdef HAVE_W32_SYSTEM
	  unsigned long nread;

	  nbytes = 0;
	  if (!ReadFile (f, buf, size, &nread, NULL))
	    {
	      int ec = (int) GetLastError ();
	      if (ec != ERROR_BROKEN_PIPE)
		{
		  rc = gpg_error_from_errno (ec);
		  log_error ("%s: read error: %s (ec=%d)\n",
                             a->fname, gpg_strerror (rc), ec);
		}
	    }
	  else if (!nread)
	    {
	      a->eof_seen = 1;
	      rc = -1;
	    }
	  else
	    {
	      nbytes = nread;
	    }

#else

	  int n;

	  nbytes = 0;
        read_more:
          do
            {
              n = read (f, buf + nbytes, size - nbytes);
            }
          while (n == -1 && errno == EINTR);
          if (n > 0)
            {
              nbytes += n;
              if (nbytes < size)
                goto read_more;
            }
          else if (!n) /* eof */
            {
              if (nbytes)
                a->delayed_rc = -1;
              else
                {
                  a->eof_seen = 1;
                  rc = -1;
                }
            }
          else /* error */
            {
              rc = gpg_error_from_syserror ();
              if (gpg_err_code (rc) != GPG_ERR_EPIPE)
                log_error ("%s: read error: %s\n", a->fname, gpg_strerror (rc));
              if (nbytes)
                {
                  a->delayed_rc = rc;
                  rc = 0;
                }
            }
#endif
	  *ret_len = nbytes;
	}
    }
  else if (control == IOBUFCTRL_FLUSH)
    {
      if (size)
	{
#ifdef HAVE_W32_SYSTEM
	  byte *p = buf;
	  unsigned long n;

	  nbytes = size;
	  do
	    {
	      if (size && !WriteFile (f, p, nbytes, &n, NULL))
		{
		  int ec = gnupg_w32_set_errno (-1);
		  rc = gpg_error_from_syserror ();
		  log_error ("%s: write error: %s (ec=%d)\n",
                             a->fname, gpg_strerror (rc), ec);
		  break;
		}
	      p += n;
	      nbytes -= n;
	    }
	  while (nbytes);
	  nbytes = p - buf;
#else
	  byte *p = buf;
	  int n;

	  nbytes = size;
	  do
	    {
	      do
		{
		  n = write (f, p, nbytes);
		}
	      while (n == -1 && errno == EINTR);
	      if (n > 0)
		{
		  p += n;
		  nbytes -= n;
		}
	    }
	  while (n != -1 && nbytes);
	  if (n == -1)
	    {
	      rc = gpg_error_from_syserror ();
	      log_error ("%s: write error: %s\n", a->fname, strerror (errno));
	    }
	  nbytes = p - buf;
#endif
	}
      *ret_len = nbytes;
    }
  else if (control == IOBUFCTRL_INIT)
    {
      a->eof_seen = 0;
      a->delayed_rc = 0;
      a->keep_open = 0;
      a->no_cache = 0;
      a->npeeked = 0;
      a->upeeked = 0;
    }
  else if (control == IOBUFCTRL_PEEK)
    {
      /* Peek on the input.  */
#ifdef HAVE_W32_SYSTEM
      unsigned long nread;

      nbytes = 0;
      if (!ReadFile (f, a->peeked, sizeof a->peeked, &nread, NULL))
        {
          int ec = (int) GetLastError ();
          if (ec != ERROR_BROKEN_PIPE)
            {
              rc = gpg_error_from_errno (ec);
              log_error ("%s: read error: %s (ec=%d)\n",
                         a->fname, gpg_strerror (rc), ec);
            }
          a->npeeked = 0;
        }
      else if (!nread)
        {
          a->eof_seen = 1;
          a->npeeked = 0;
        }
      else
        {
          a->npeeked = nread;
        }

#else /* Unix */

      int n;

    peek_more:
      do
        {
          n = read (f, a->peeked + a->npeeked, sizeof a->peeked - a->npeeked);
        }
      while (n == -1 && errno == EINTR);
      if (n > 0)
        {
          a->npeeked += n;
          if (a->npeeked < sizeof a->peeked)
            goto peek_more;
        }
      else if (!n) /* eof */
        {
          if (a->npeeked)
            a->delayed_rc = -1;
          else
            a->eof_seen = 1;
        }
      else /* error */
        {
          rc = gpg_error_from_syserror ();
          if (gpg_err_code (rc) != GPG_ERR_EPIPE)
            log_error ("%s: read error: %s\n", a->fname, gpg_strerror (rc));
          if (a->npeeked)
            a->delayed_rc = rc;
        }
#endif /* Unix */

      size = a->npeeked < size? a->npeeked : size;
      memcpy (buf, a->peeked, size);
      *ret_len = size;
      rc = 0;  /* Return success - the user needs to check ret_len.  */
    }
  else if (control == IOBUFCTRL_DESC)
    {
      mem2str (buf, "file_filter(fd)", *ret_len);
    }
  else if (control == IOBUFCTRL_FREE)
    {
      if (f != FD_FOR_STDIN && f != FD_FOR_STDOUT)
	{
	  if (DBG_IOBUF)
	    log_debug ("%s: close fd/handle %d\n", a->fname, FD_DBG (f));
	  if (!a->keep_open)
	    fd_cache_close (a->no_cache ? NULL : a->fname, f);
	}
      xfree (a); /* We can free our context now. */
    }

  return rc;
}


/* Similar to file_filter but using the estream system.  */
static int
file_es_filter (void *opaque, int control, iobuf_t chain, byte * buf,
                size_t * ret_len)
{
  file_es_filter_ctx_t *a = opaque;
  estream_t f = a->fp;
  size_t size = *ret_len;
  size_t nbytes = 0;
  int rc = 0;

  (void)chain; /* Not used.  */

  if (control == IOBUFCTRL_UNDERFLOW)
    {
      log_assert (size); /* We need a buffer.  */
      if (a->eof_seen)
	{
	  rc = -1;
	  *ret_len = 0;
	}
      else if (a->use_readlimit)
	{
          nbytes = 0;
          if (!a->readlimit)
	    {			/* eof */
	      a->eof_seen = 1;
	      rc = -1;
	    }
          else
            {
              if (size > a->readlimit)
                size = a->readlimit;
              rc = es_read (f, buf, size, &nbytes);
              if (rc == -1)
                {			/* error */
                  rc = gpg_error_from_syserror ();
                  log_error ("%s: read error: %s\n", a->fname,strerror (errno));
                }
              else if (!nbytes)
                {			/* eof */
                  a->eof_seen = 1;
                  rc = -1;
                }
              else
                a->readlimit -= nbytes;
            }
	  *ret_len = nbytes;
	}
      else
	{
          nbytes = 0;
          rc = es_read (f, buf, size, &nbytes);
	  if (rc == -1)
	    {			/* error */
              rc = gpg_error_from_syserror ();
              log_error ("%s: read error: %s\n", a->fname, strerror (errno));
	    }
	  else if (!nbytes)
	    {			/* eof */
	      a->eof_seen = 1;
	      rc = -1;
	    }
	  *ret_len = nbytes;
	}
    }
  else if (control == IOBUFCTRL_FLUSH)
    {
      if (size)
	{
	  byte *p = buf;
	  size_t nwritten;

	  nbytes = size;
	  do
	    {
              nwritten = 0;
              if (es_write (f, p, nbytes, &nwritten))
                {
                  rc = gpg_error_from_syserror ();
                  log_error ("%s: write error: %s\n",
                             a->fname, strerror (errno));
                  break;
                }
              p += nwritten;
              nbytes -= nwritten;
	    }
	  while (nbytes);
	  nbytes = p - buf;
	}
      *ret_len = nbytes;
    }
  else if (control == IOBUFCTRL_INIT)
    {
      a->eof_seen = 0;
      a->no_cache = 0;
    }
  else if (control == IOBUFCTRL_DESC)
    {
      mem2str (buf, "estream_filter", *ret_len);
    }
  else if (control == IOBUFCTRL_FREE)
    {
      if (f != es_stdin && f != es_stdout)
	{
	  if (DBG_IOBUF)
	    log_debug ("%s: es_fclose %p\n", a->fname, f);
	  if (!a->keep_open)
	    es_fclose (f);
	}
      f = NULL;
      xfree (a); /* We can free our context now. */
    }

  return rc;
}


#ifdef HAVE_W32_SYSTEM
/* Because network sockets are special objects under Lose32 we have to
   use a dedicated filter for them. */
static int
sock_filter (void *opaque, int control, iobuf_t chain, byte * buf,
	     size_t * ret_len)
{
  sock_filter_ctx_t *a = opaque;
  size_t size = *ret_len;
  size_t nbytes = 0;
  int rc = 0;

  (void)chain;

  if (control == IOBUFCTRL_UNDERFLOW)
    {
      log_assert (size);		/* need a buffer */
      if (a->eof_seen)
	{
	  rc = -1;
	  *ret_len = 0;
	}
      else
	{
	  int nread;

	  nread = recv (a->sock, buf, size, 0);
	  if (nread == SOCKET_ERROR)
	    {
	      int ec = (int) WSAGetLastError ();
	      rc = gpg_error_from_errno (ec);
	      log_error ("socket read error: ec=%d\n", ec);
	    }
	  else if (!nread)
	    {
	      a->eof_seen = 1;
	      rc = -1;
	    }
	  else
	    {
	      nbytes = nread;
	    }
	  *ret_len = nbytes;
	}
    }
  else if (control == IOBUFCTRL_FLUSH)
    {
      if (size)
	{
	  byte *p = buf;
	  int n;

	  nbytes = size;
	  do
	    {
	      n = send (a->sock, p, nbytes, 0);
	      if (n == SOCKET_ERROR)
		{
		  int ec = (int) WSAGetLastError ();
		  gnupg_w32_set_errno (ec);
		  rc = gpg_error_from_syserror ();
		  log_error ("socket write error: ec=%d\n", ec);
		  break;
		}
	      p += n;
	      nbytes -= n;
	    }
	  while (nbytes);
	  nbytes = p - buf;
	}
      *ret_len = nbytes;
    }
  else if (control == IOBUFCTRL_INIT)
    {
      a->eof_seen = 0;
      a->keep_open = 0;
      a->no_cache = 0;
    }
  else if (control == IOBUFCTRL_DESC)
    {
      mem2str (buf, "sock_filter", *ret_len);
    }
  else if (control == IOBUFCTRL_FREE)
    {
      if (!a->keep_open)
	closesocket (a->sock);
      xfree (a);		/* we can free our context now */
    }
  return rc;
}
#endif /*HAVE_W32_SYSTEM*/

/****************
 * This is used to implement the block write mode.
 * Block reading is done on a byte by byte basis in readbyte(),
 * without a filter
 */
static int
block_filter (void *opaque, int control, iobuf_t chain, byte * buffer,
	      size_t * ret_len)
{
  block_filter_ctx_t *a = opaque;
  char *buf = (char *)buffer;
  size_t size = *ret_len;
  int c, needed, rc = 0;
  char *p;

  if (control == IOBUFCTRL_UNDERFLOW)
    {
      size_t n = 0;

      p = buf;
      log_assert (size);	/* need a buffer */
      if (a->eof)		/* don't read any further */
	rc = -1;
      while (!rc && size)
	{
	  if (!a->size)
	    {			/* get the length bytes */
	      if (a->partial == 2)
		{
		  a->eof = 1;
		  if (!n)
		    rc = -1;
		  break;
		}
	      else if (a->partial)
		{
		  /* These OpenPGP introduced huffman like encoded length
		   * bytes are really a mess :-( */
		  if (a->first_c)
		    {
		      c = a->first_c;
		      a->first_c = 0;
		    }
		  else if ((c = iobuf_get (chain)) == -1)
		    {
		      log_error ("block_filter: 1st length byte missing\n");
		      rc = GPG_ERR_BAD_DATA;
		      break;
		    }
		  if (c < 192)
		    {
		      a->size = c;
		      a->partial = 2;
		      if (!a->size)
			{
			  a->eof = 1;
			  if (!n)
			    rc = -1;
			  break;
			}
		    }
		  else if (c < 224)
		    {
		      a->size = (c - 192) * 256;
		      if ((c = iobuf_get (chain)) == -1)
			{
			  log_error
			    ("block_filter: 2nd length byte missing\n");
			  rc = GPG_ERR_BAD_DATA;
			  break;
			}
		      a->size += c + 192;
		      a->partial = 2;
		      if (!a->size)
			{
			  a->eof = 1;
			  if (!n)
			    rc = -1;
			  break;
			}
		    }
		  else if (c == 255)
		    {
                      size_t len = 0;
                      int i;

                      for (i = 0; i < 4; i++)
                        if ((c = iobuf_get (chain)) == -1)
                          break;
                        else
                          len = ((len << 8) | c);

                      if (i < 4)
			{
			  log_error ("block_filter: invalid 4 byte length\n");
			  rc = GPG_ERR_BAD_DATA;
			  break;
			}
                      a->size = len;
                      a->partial = 2;
                      if (!a->size)
                        {
                          a->eof = 1;
                          if (!n)
                            rc = -1;
                          break;
			}
		    }
		  else
		    { /* Next partial body length. */
		      a->size = 1 << (c & 0x1f);
		    }
		  /*  log_debug("partial: ctx=%p c=%02x size=%u\n", a, c, a->size); */
		}
	      else
		BUG ();
	    }

	  while (!rc && size && a->size)
	    {
	      needed = size < a->size ? size : a->size;
	      c = iobuf_read (chain, p, needed);
	      if (c < needed)
		{
		  if (c == -1)
		    c = 0;
		  log_error
		    ("block_filter %p: read error (size=%lu,a->size=%lu)\n",
		     a, (ulong) size + c, (ulong) a->size + c);
		  rc = GPG_ERR_BAD_DATA;
		}
	      else
		{
		  size -= c;
		  a->size -= c;
		  p += c;
		  n += c;
		}
	    }
	}
      *ret_len = n;
    }
  else if (control == IOBUFCTRL_FLUSH)
    {
      if (a->partial)
	{			/* the complicated openpgp scheme */
	  size_t blen, n, nbytes = size + a->buflen;

	  log_assert (a->buflen <= OP_MIN_PARTIAL_CHUNK);
	  if (nbytes < OP_MIN_PARTIAL_CHUNK)
	    {
	      /* not enough to write a partial block out; so we store it */
	      if (!a->buffer)
		a->buffer = xmalloc (OP_MIN_PARTIAL_CHUNK);
	      memcpy (a->buffer + a->buflen, buf, size);
	      a->buflen += size;
	    }
	  else
	    {			/* okay, we can write out something */
	      /* do this in a loop to use the most efficient block lengths */
	      p = buf;
	      do
		{
		  /* find the best matching block length - this is limited
		   * by the size of the internal buffering */
		  for (blen = OP_MIN_PARTIAL_CHUNK * 2,
		       c = OP_MIN_PARTIAL_CHUNK_2POW + 1; blen <= nbytes;
		       blen *= 2, c++)
		    ;
		  blen /= 2;
		  c--;
		  /* write the partial length header */
		  log_assert (c <= 0x1f);	/*;-) */
		  c |= 0xe0;
		  iobuf_put (chain, c);
		  if ((n = a->buflen))
		    {		/* write stuff from the buffer */
		      log_assert (n == OP_MIN_PARTIAL_CHUNK);
		      if (iobuf_write (chain, a->buffer, n))
			rc = gpg_error_from_syserror ();
		      a->buflen = 0;
		      nbytes -= n;
		    }
		  if ((n = nbytes) > blen)
		    n = blen;
		  if (n && iobuf_write (chain, p, n))
		    rc = gpg_error_from_syserror ();
		  p += n;
		  nbytes -= n;
		}
	      while (!rc && nbytes >= OP_MIN_PARTIAL_CHUNK);
	      /* store the rest in the buffer */
	      if (!rc && nbytes)
		{
		  log_assert (!a->buflen);
		  log_assert (nbytes < OP_MIN_PARTIAL_CHUNK);
		  if (!a->buffer)
		    a->buffer = xmalloc (OP_MIN_PARTIAL_CHUNK);
		  memcpy (a->buffer, p, nbytes);
		  a->buflen = nbytes;
		}
	    }
	}
      else
	BUG ();
    }
  else if (control == IOBUFCTRL_INIT)
    {
      if (DBG_IOBUF)
	log_debug ("init block_filter %p\n", a);
      if (a->partial)
	a->count = 0;
      else if (a->use == IOBUF_INPUT)
	a->count = a->size = 0;
      else
	a->count = a->size;	/* force first length bytes */
      a->eof = 0;
      a->buffer = NULL;
      a->buflen = 0;
    }
  else if (control == IOBUFCTRL_DESC)
    {
      mem2str (buf, "block_filter", *ret_len);
    }
  else if (control == IOBUFCTRL_FREE)
    {
      if (a->use == IOBUF_OUTPUT)
	{			/* write the end markers */
	  if (a->partial)
	    {
	      u32 len;
	      /* write out the remaining bytes without a partial header
	       * the length of this header may be 0 - but if it is
	       * the first block we are not allowed to use a partial header
	       * and frankly we can't do so, because this length must be
	       * a power of 2. This is _really_ complicated because we
	       * have to check the possible length of a packet prior
	       * to it's creation: a chain of filters becomes complicated
	       * and we need a lot of code to handle compressed packets etc.
	       *   :-(((((((
	       */
	      /* construct header */
	      len = a->buflen;
	      /*log_debug("partial: remaining length=%u\n", len ); */
	      if (len < 192)
		rc = iobuf_put (chain, len);
	      else if (len < 8384)
		{
		  if (!(rc = iobuf_put (chain, ((len - 192) / 256) + 192)))
		    rc = iobuf_put (chain, ((len - 192) % 256));
		}
	      else
		{		/* use a 4 byte header */
		  if (!(rc = iobuf_put (chain, 0xff)))
		    if (!(rc = iobuf_put (chain, (len >> 24) & 0xff)))
		      if (!(rc = iobuf_put (chain, (len >> 16) & 0xff)))
			if (!(rc = iobuf_put (chain, (len >> 8) & 0xff)))
			  rc = iobuf_put (chain, len & 0xff);
		}
	      if (!rc && len)
		rc = iobuf_write (chain, a->buffer, len);
	      if (rc)
		{
		  log_error ("block_filter: write error: %s\n",
			     strerror (errno));
		  rc = gpg_error_from_syserror ();
		}
	      xfree (a->buffer);
	      a->buffer = NULL;
	      a->buflen = 0;
	    }
	  else
	    BUG ();
	}
      else if (a->size)
	{
	  log_error ("block_filter: pending bytes!\n");
	}
      if (DBG_IOBUF)
	log_debug ("free block_filter %p\n", a);
      xfree (a);		/* we can free our context now */
    }

  return rc;
}


/* Change the default size for all IOBUFs to KILOBYTE.  This needs to
 * be called before any iobufs are used and can only be used once.
 * Returns the current value.  Using 0 has no effect except for
 * returning the current value.  */
unsigned int
iobuf_set_buffer_size (unsigned int kilobyte)
{
  static int used;

  if (!used && kilobyte)
    {
      if (kilobyte < 4)
        kilobyte = 4;
      else if (kilobyte > 16*1024)
        kilobyte = 16*1024;

      iobuf_buffer_size = kilobyte * 1024;
      used = 1;
    }
  return iobuf_buffer_size / 1024;
}


#define MAX_IOBUF_DESC 32
/*
 * Fill the buffer by the description of iobuf A.
 * The buffer size should be MAX_IOBUF_DESC (or larger).
 * Returns BUF as (const char *).
 */
static const char *
iobuf_desc (iobuf_t a, byte *buf)
{
  size_t len = MAX_IOBUF_DESC;

  if (! a || ! a->filter)
    memcpy (buf, "?", 2);
  else
    a->filter (a->filter_ov, IOBUFCTRL_DESC, NULL, buf, &len);

  return buf;
}

static void
print_chain (iobuf_t a)
{
  if (!DBG_IOBUF)
    return;
  for (; a; a = a->chain)
    {
      byte desc[MAX_IOBUF_DESC];

      log_debug ("iobuf chain: %d.%d '%s' filter_eof=%d start=%d len=%d\n",
		 a->no, a->subno, iobuf_desc (a, desc), a->filter_eof,
		 (int) a->d.start, (int) a->d.len);
    }
}

int
iobuf_print_chain (iobuf_t a)
{
  print_chain (a);
  return 0;
}

iobuf_t
iobuf_alloc (int use, size_t bufsize)
{
  iobuf_t a;
  static int number = 0;

  log_assert (use == IOBUF_INPUT || use == IOBUF_INPUT_TEMP
              || use == IOBUF_OUTPUT || use == IOBUF_OUTPUT_TEMP);
  if (bufsize == 0)
    {
      log_bug ("iobuf_alloc() passed a bufsize of 0!\n");
      bufsize = iobuf_buffer_size;
    }

  a = xcalloc (1, sizeof *a);
  a->use = use;
  a->d.buf = xmalloc (bufsize);
  a->d.size = bufsize;
  a->e_d.buf = NULL;
  a->e_d.len = 0;
  a->e_d.used = 0;
  a->e_d.preferred = 0;
  a->no = ++number;
  a->subno = 0;
  a->real_fname = NULL;
  return a;
}

int
iobuf_close (iobuf_t a)
{
  iobuf_t a_chain;
  size_t dummy_len = 0;
  int rc = 0;

  for (; a; a = a_chain)
    {
      byte desc[MAX_IOBUF_DESC];
      int rc2 = 0;

      a_chain = a->chain;

      if (a->use == IOBUF_OUTPUT && (rc = filter_flush (a)))
	log_error ("filter_flush failed on close: %s\n", gpg_strerror (rc));

      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: close '%s'\n",
		   a->no, a->subno, iobuf_desc (a, desc));

      if (a->filter && (rc2 = a->filter (a->filter_ov, IOBUFCTRL_FREE,
					 a->chain, NULL, &dummy_len)))
	log_error ("IOBUFCTRL_FREE failed on close: %s\n", gpg_strerror (rc));
      if (! rc && rc2)
	/* Whoops!  An error occurred.  Save it in RC if we haven't
	   already recorded an error.  */
	rc = rc2;

      xfree (a->real_fname);
      if (a->d.buf)
	{
	  memset (a->d.buf, 0, a->d.size);	/* erase the buffer */
	  xfree (a->d.buf);
	}
      xfree (a);
    }
  return rc;
}

int
iobuf_cancel (iobuf_t a)
{
  const char *s;
  iobuf_t a2;
  int rc;
#if defined(HAVE_W32_SYSTEM) || defined(__riscos__)
  char *remove_name = NULL;
#endif

  if (a && a->use == IOBUF_OUTPUT)
    {
      s = iobuf_get_real_fname (a);
      if (s && *s)
	{
#if defined(HAVE_W32_SYSTEM) || defined(__riscos__)
	  remove_name = xstrdup (s);
#else
	  remove (s);
#endif
	}
    }

  /* send a cancel message to all filters */
  for (a2 = a; a2; a2 = a2->chain)
    {
      size_t dummy = 0;
      if (a2->filter)
	a2->filter (a2->filter_ov, IOBUFCTRL_CANCEL, a2->chain, NULL, &dummy);
    }

  rc = iobuf_close (a);
#if defined(HAVE_W32_SYSTEM) || defined(__riscos__)
  if (remove_name)
    {
      /* Argg, MSDOS does not allow removing open files.  So
       * we have to do it here */
      gnupg_remove (remove_name);

      xfree (remove_name);
    }
#endif
  return rc;
}


iobuf_t
iobuf_temp (void)
{
  return iobuf_alloc (IOBUF_OUTPUT_TEMP, iobuf_buffer_size);
}

iobuf_t
iobuf_temp_with_content (const char *buffer, size_t length)
{
  iobuf_t a;
  int i;

  a = iobuf_alloc (IOBUF_INPUT_TEMP, length);
  log_assert (length == a->d.size);
  /* memcpy (a->d.buf, buffer, length); */
  for (i=0; i < length; i++)
    a->d.buf[i] = buffer[i];
  a->d.len = length;

  return a;
}


int
iobuf_is_pipe_filename (const char *fname)
{
  if (!fname || (*fname=='-' && !fname[1]) )
    return 1;
  return gnupg_check_special_filename (fname) != GNUPG_INVALID_FD;
}


static iobuf_t
do_open (const char *fname, int special_filenames,
	 int use, const char *opentype, int mode700)
{
  iobuf_t a;
  gnupg_fd_t fp;
  file_filter_ctx_t *fcx;
  size_t len = 0;
  int print_only = 0;
  gnupg_fd_t fd;
  byte desc[MAX_IOBUF_DESC];

  log_assert (use == IOBUF_INPUT || use == IOBUF_OUTPUT);

  if (special_filenames
      /* NULL or '-'.  */
      && (!fname || (*fname == '-' && !fname[1])))
    {
      if (use == IOBUF_INPUT)
	{
	  fp = FD_FOR_STDIN;
	  fname = "[stdin]";
	}
      else
	{
	  fp = FD_FOR_STDOUT;
	  fname = "[stdout]";
	}
      print_only = 1;
    }
  else if (!fname)
    return NULL;
  else if (special_filenames
           && (fd = gnupg_check_special_filename (fname)) != GNUPG_INVALID_FD)
    return do_iobuf_fdopen (fd, opentype, 0);
  else
    {
      if (use == IOBUF_INPUT)
	fp = fd_cache_open (fname, opentype);
      else
	fp = direct_open (fname, opentype, mode700);
      if (fp == GNUPG_INVALID_FD)
	return NULL;
    }

  a = iobuf_alloc (use, iobuf_buffer_size);
  fcx = xmalloc (sizeof *fcx + strlen (fname));
  fcx->fp = fp;
  fcx->print_only_name = print_only;
  strcpy (fcx->fname, fname);
  if (!print_only)
    a->real_fname = xstrdup (fname);
  a->filter = file_filter;
  a->filter_ov = fcx;
  file_filter (fcx, IOBUFCTRL_INIT, NULL, NULL, &len);
  if (DBG_IOBUF)
    log_debug ("iobuf-%d.%d: open '%s' desc=%s fd=%d\n",
	       a->no, a->subno, fname, iobuf_desc (a, desc),
               FD_DBG (fcx->fp));

  return a;
}

iobuf_t
iobuf_open (const char *fname)
{
  return do_open (fname, 1, IOBUF_INPUT, "rb", 0);
}

iobuf_t
iobuf_create (const char *fname, int mode700)
{
  return do_open (fname, 1, IOBUF_OUTPUT, "wb", mode700);
}

iobuf_t
iobuf_openrw (const char *fname)
{
  return do_open (fname, 0, IOBUF_OUTPUT, "r+b", 0);
}


static iobuf_t
do_iobuf_fdopen (gnupg_fd_t fp, const char *mode, int keep_open)
{
  iobuf_t a;
  file_filter_ctx_t *fcx;
  size_t len = 0;

  a = iobuf_alloc (strchr (mode, 'w') ? IOBUF_OUTPUT : IOBUF_INPUT,
		   iobuf_buffer_size);
  fcx = xmalloc (sizeof *fcx + 20);
  fcx->fp = fp;
  fcx->print_only_name = 1;
  fcx->keep_open = keep_open;
  sprintf (fcx->fname, "[fd %d]", FD_DBG (fp));
  a->filter = file_filter;
  a->filter_ov = fcx;
  file_filter (fcx, IOBUFCTRL_INIT, NULL, NULL, &len);
  if (DBG_IOBUF)
    log_debug ("iobuf-%d.%d: fdopen%s '%s'\n",
               a->no, a->subno, keep_open? "_nc":"", fcx->fname);
  iobuf_ioctl (a, IOBUF_IOCTL_NO_CACHE, 1, NULL);
  return a;
}


iobuf_t
iobuf_fdopen (gnupg_fd_t fp, const char *mode)
{
  return do_iobuf_fdopen (fp, mode, 0);
}

iobuf_t
iobuf_fdopen_nc (gnupg_fd_t fp, const char *mode)
{
  return do_iobuf_fdopen (fp, mode, 1);
}


iobuf_t
iobuf_esopen (estream_t estream, const char *mode, int keep_open,
              size_t readlimit)
{
  iobuf_t a;
  file_es_filter_ctx_t *fcx;
  size_t len = 0;

  a = iobuf_alloc (strchr (mode, 'w') ? IOBUF_OUTPUT : IOBUF_INPUT,
		   iobuf_buffer_size);
  fcx = xtrymalloc (sizeof *fcx + 30);
  fcx->fp = estream;
  fcx->print_only_name = 1;
  fcx->keep_open = keep_open;
  fcx->readlimit = readlimit;
  fcx->use_readlimit = !!readlimit;
  snprintf (fcx->fname, 30, "[fd %p]", estream);
  a->filter = file_es_filter;
  a->filter_ov = fcx;
  file_es_filter (fcx, IOBUFCTRL_INIT, NULL, NULL, &len);
  if (DBG_IOBUF)
    log_debug ("iobuf-%d.%d: esopen%s '%s'\n",
               a->no, a->subno, keep_open? "_nc":"", fcx->fname);
  return a;
}


iobuf_t
iobuf_sockopen (int fd, const char *mode)
{
  iobuf_t a;
#ifdef HAVE_W32_SYSTEM
  sock_filter_ctx_t *scx;
  size_t len;

  a = iobuf_alloc (strchr (mode, 'w') ? IOBUF_OUTPUT : IOBUF_INPUT,
		   iobuf_buffer_size);
  scx = xmalloc (sizeof *scx + 25);
  scx->sock = fd;
  scx->print_only_name = 1;
  sprintf (scx->fname, "[sock %d]", fd);
  a->filter = sock_filter;
  a->filter_ov = scx;
  sock_filter (scx, IOBUFCTRL_INIT, NULL, NULL, &len);
  if (DBG_IOBUF)
    log_debug ("iobuf-%d.%d: sockopen '%s'\n", a->no, a->subno, scx->fname);
  iobuf_ioctl (a, IOBUF_IOCTL_NO_CACHE, 1, NULL);
#else
  a = do_iobuf_fdopen (fd, mode, 0);
#endif
  return a;
}

int
iobuf_ioctl (iobuf_t a, iobuf_ioctl_t cmd, int intval, void *ptrval)
{
  byte desc[MAX_IOBUF_DESC];

  if (cmd == IOBUF_IOCTL_KEEP_OPEN)
    {
      /* Keep system filepointer/descriptor open.  This was used in
         the past by http.c; this ioctl is not directly used
         anymore.  */
      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: ioctl '%s' keep_open=%d\n",
		   a ? a->no : -1, a ? a->subno : -1, iobuf_desc (a, desc),
		   intval);
      for (; a; a = a->chain)
	if (!a->chain && a->filter == file_filter)
	  {
	    file_filter_ctx_t *b = a->filter_ov;
	    b->keep_open = intval;
	    return 0;
	  }
#ifdef HAVE_W32_SYSTEM
	else if (!a->chain && a->filter == sock_filter)
	  {
	    sock_filter_ctx_t *b = a->filter_ov;
	    b->keep_open = intval;
	    return 0;
	  }
#endif
    }
  else if (cmd == IOBUF_IOCTL_INVALIDATE_CACHE)
    {
      if (DBG_IOBUF)
	log_debug ("iobuf-*.*: ioctl '%s' invalidate\n",
		   ptrval ? (char *) ptrval : "?");
      if (!a && !intval && ptrval)
	{
	  if (fd_cache_invalidate (ptrval))
            return -1;
	  return 0;
	}
    }
  else if (cmd == IOBUF_IOCTL_NO_CACHE)
    {
      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: ioctl '%s' no_cache=%d\n",
		   a ? a->no : -1, a ? a->subno : -1, iobuf_desc (a, desc),
		   intval);
      for (; a; a = a->chain)
	if (!a->chain && a->filter == file_filter)
	  {
	    file_filter_ctx_t *b = a->filter_ov;
	    b->no_cache = intval;
	    return 0;
	  }
#ifdef HAVE_W32_SYSTEM
	else if (!a->chain && a->filter == sock_filter)
	  {
	    sock_filter_ctx_t *b = a->filter_ov;
	    b->no_cache = intval;
	    return 0;
	  }
#endif
    }
  else if (cmd == IOBUF_IOCTL_FSYNC)
    {
      /* Do a fsync on the open fd and return any errors to the caller
         of iobuf_ioctl.  Note that we work on a file name here. */
      if (DBG_IOBUF)
        log_debug ("iobuf-*.*: ioctl '%s' fsync\n",
                   ptrval? (const char*)ptrval:"<null>");

      if (!a && !intval && ptrval)
        {
          return fd_cache_synchronize (ptrval);
        }
    }
  else if (cmd == IOBUF_IOCTL_PEEK)
    {
      /* Peek at a justed opened file.  Use this only directly after a
       * file has been opened for reading.  Don't use it after you did
       * a seek.  This works only if just file filter has been
       * pushed.  Expects a buffer with size INTVAL at PTRVAL and returns
       * the number of bytes put into the buffer.  */
      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: ioctl '%s' peek\n",
		   a ? a->no : -1, a ? a->subno : -1, iobuf_desc (a, desc));
      if (a->filter == file_filter && ptrval && intval)
        {
          file_filter_ctx_t *fcx = a->filter_ov;
          size_t len = intval;

          if (!file_filter (fcx, IOBUFCTRL_PEEK, NULL, ptrval, &len))
            return (int)len;
        }
    }


  return -1;
}


/****************
 * Register an i/o filter.
 */
int
iobuf_push_filter (iobuf_t a,
		   int (*f) (void *opaque, int control,
			     iobuf_t chain, byte * buf, size_t * len),
                   void *ov)
{
  return iobuf_push_filter2 (a, f, ov, 0);
}

int
iobuf_push_filter2 (iobuf_t a,
		    int (*f) (void *opaque, int control,
			      iobuf_t chain, byte * buf, size_t * len),
		    void *ov, int rel_ov)
{
  iobuf_t b;
  size_t dummy_len = 0;
  int rc = 0;

  if (a->use == IOBUF_OUTPUT && (rc = filter_flush (a)))
    return rc;

  if (a->subno >= MAX_NESTING_FILTER)
    {
      log_error ("i/o filter too deeply nested - corrupted data?\n");
      return GPG_ERR_BAD_DATA;
    }

  /* We want to create a new filter and put it in front of A.  A
     simple implementation would do:

       b = iobuf_alloc (...);
       b->chain = a;
       return a;

     This is a bit problematic: A is the head of the pipeline and
     there are potentially many pointers to it.  Requiring the caller
     to update all of these pointers is a burden.

     An alternative implementation would add a level of indirection.
     For instance, we could use a pipeline object, which contains a
     pointer to the first filter in the pipeline.  This is not what we
     do either.

     Instead, we allocate a new buffer (B) and copy the first filter's
     state into that and use the initial buffer (A) for the new
     filter.  One limitation of this approach is that it is not
     practical to maintain a pointer to a specific filter's state.

     Before:

           A
           |
           v 0x100               0x200
           +----------+          +----------+
           | filter x |--------->| filter y |---->....
           +----------+          +----------+

     After:           B
                      |
                      v 0x300
                      +----------+
           A          | filter x |
           |          +----------+
           v 0x100    ^          v 0x200
           +----------+          +----------+
           | filter w |          | filter y |---->....
           +----------+          +----------+

     Note: filter x's address changed from 0x100 to 0x300, but A still
     points to the head of the pipeline.
  */

  b = xmalloc (sizeof *b);
  memcpy (b, a, sizeof *b);
  /* fixme: it is stupid to keep a copy of the name at every level
   * but we need the name somewhere because the name known by file_filter
   * may have been released when we need the name of the file */
  b->real_fname = a->real_fname ? xstrdup (a->real_fname) : NULL;
  /* remove the filter stuff from the new stream */
  a->filter = NULL;
  a->filter_ov = NULL;
  a->filter_ov_owner = 0;
  a->filter_eof = 0;
  if (a->use == IOBUF_OUTPUT_TEMP)
    /* A TEMP filter buffers any data sent to it; it does not forward
       any data down the pipeline.  If we add a new filter to the
       pipeline, it shouldn't also buffer data.  It should send it
       downstream to be buffered.  Thus, the correct type for a filter
       added in front of an IOBUF_OUTPUT_TEMP filter is IOBUF_OUPUT, not
       IOBUF_OUTPUT_TEMP.  */
    {
      a->use = IOBUF_OUTPUT;

      /* When pipeline is written to, the temp buffer's size is
	 increased accordingly.  We don't need to allocate a 10 MB
	 buffer for a non-terminal filter.  Just use the default
	 size.  */
      a->d.size = iobuf_buffer_size;
    }
  else if (a->use == IOBUF_INPUT_TEMP)
    /* Same idea as above.  */
    {
      a->use = IOBUF_INPUT;
      a->d.size = iobuf_buffer_size;
    }

  /* The new filter (A) gets a new buffer.

     If the pipeline is an output or temp pipeline, then giving the
     buffer to the new filter means that data that was written before
     the filter was pushed gets sent to the filter.  That's clearly
     wrong.

     If the pipeline is an input pipeline, then giving the buffer to
     the new filter (A) means that data that has read from (B), but
     not yet read from the pipeline won't be processed by the new
     filter (A)!  That's certainly not what we want.  */
  a->d.buf = xmalloc (a->d.size);
  a->d.len = 0;
  a->d.start = 0;

  /* disable nlimit for the new stream */
  a->ntotal = b->ntotal + b->nbytes;
  a->nlimit = a->nbytes = 0;
  a->nofast = 0;
  /* make a link from the new stream to the original stream */
  a->chain = b;

  /* setup the function on the new stream */
  a->filter = f;
  a->filter_ov = ov;
  a->filter_ov_owner = rel_ov;

  a->subno = b->subno + 1;

  if (DBG_IOBUF)
    {
      byte desc[MAX_IOBUF_DESC];
      log_debug ("iobuf-%d.%d: push '%s'\n",
		 a->no, a->subno, iobuf_desc (a, desc));
      print_chain (a);
    }

  /* now we can initialize the new function if we have one */
  if (a->filter && (rc = a->filter (a->filter_ov, IOBUFCTRL_INIT, a->chain,
				    NULL, &dummy_len)))
    log_error ("IOBUFCTRL_INIT failed: %s\n", gpg_strerror (rc));
  return rc;
}

/****************
 * Remove an i/o filter.
 */
int
iobuf_pop_filter (iobuf_t a, int (*f) (void *opaque, int control,
                                       iobuf_t chain, byte * buf, size_t * len),
                  void *ov)
{
  iobuf_t b;
  size_t dummy_len = 0;
  int rc = 0;
  byte desc[MAX_IOBUF_DESC];

  if (DBG_IOBUF)
    log_debug ("iobuf-%d.%d: pop '%s'\n",
	       a->no, a->subno, iobuf_desc (a, desc));
  if (a->use == IOBUF_INPUT_TEMP || a->use == IOBUF_OUTPUT_TEMP)
    {
      /* This should be the last filter in the pipeline.  */
      log_assert (! a->chain);
      return 0;
    }
  if (!a->filter)
    {				/* this is simple */
      b = a->chain;
      log_assert (b);
      xfree (a->d.buf);
      xfree (a->real_fname);
      memcpy (a, b, sizeof *a);
      xfree (b);
      return 0;
    }
  for (b = a; b; b = b->chain)
    if (b->filter == f && (!ov || b->filter_ov == ov))
      break;
  if (!b)
    log_bug ("iobuf_pop_filter(): filter function not found\n");

  /* flush this stream if it is an output stream */
  if (a->use == IOBUF_OUTPUT && (rc = filter_flush (b)))
    {
      log_error ("filter_flush failed in iobuf_pop_filter: %s\n",
                 gpg_strerror (rc));
      return rc;
    }
  /* and tell the filter to free it self */
  if (b->filter && (rc = b->filter (b->filter_ov, IOBUFCTRL_FREE, b->chain,
				    NULL, &dummy_len)))
    {
      log_error ("IOBUFCTRL_FREE failed: %s\n", gpg_strerror (rc));
      return rc;
    }
  if (b->filter_ov && b->filter_ov_owner)
    {
      xfree (b->filter_ov);
      b->filter_ov = NULL;
    }


  /* and see how to remove it */
  if (a == b && !b->chain)
    log_bug ("can't remove the last filter from the chain\n");
  else if (a == b)
    {				/* remove the first iobuf from the chain */
      /* everything from b is copied to a. This is save because
       * a flush has been done on the to be removed entry
       */
      b = a->chain;
      xfree (a->d.buf);
      xfree (a->real_fname);
      memcpy (a, b, sizeof *a);
      xfree (b);
      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: popped filter\n", a->no, a->subno);
    }
  else if (!b->chain)
    {				/* remove the last iobuf from the chain */
      log_bug ("Ohh jeee, trying to remove a head filter\n");
    }
  else
    {				/* remove an intermediate iobuf from the chain */
      log_bug ("Ohh jeee, trying to remove an intermediate filter\n");
    }

  return rc;
}


/****************
 * read underflow: read at least one byte into the buffer and return
 * the first byte or -1 on EOF.
 */
static int
underflow (iobuf_t a, int clear_pending_eof)
{
  return underflow_target (a, clear_pending_eof, 1);
}


/****************
 * read underflow: read TARGET bytes into the buffer and return
 * the first byte or -1 on EOF.
 */
static int
underflow_target (iobuf_t a, int clear_pending_eof, size_t target)
{
  size_t len;
  int rc;

  if (DBG_IOBUF)
    log_debug ("iobuf-%d.%d: underflow: buffer size: %d; still buffered: %d => space for %d bytes\n",
	       a->no, a->subno,
	       (int) a->d.size, (int) (a->d.len - a->d.start),
	       (int) (a->d.size - (a->d.len - a->d.start)));

  if (a->use == IOBUF_INPUT_TEMP)
    /* By definition, there isn't more data to read into the
       buffer.  */
    return -1;

  log_assert (a->use == IOBUF_INPUT);

  a->e_d.used = 0;

  /* If there is still some buffered data, then move it to the start
     of the buffer and try to fill the end of the buffer.  (This is
     useful if we are called from iobuf_peek().)  */
  log_assert (a->d.start <= a->d.len);
  a->d.len -= a->d.start;
  if (a->d.len)
    memmove (a->d.buf, &a->d.buf[a->d.start], a->d.len);
  a->d.start = 0;

  if (a->d.len < target && a->filter_eof)
    /* The last time we tried to read from this filter, we got an EOF.
       We couldn't return the EOF, because there was buffered data.
       Since there is no longer any buffered data, return the
       error.  */
    {
      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: underflow: eof (pending eof)\n",
		   a->no, a->subno);
      if (! clear_pending_eof)
	return -1;

      if (a->chain)
	/* A filter follows this one.  Free this filter.  */
	{
	  iobuf_t b = a->chain;
	  if (DBG_IOBUF)
	    log_debug ("iobuf-%d.%d: filter popped (pending EOF returned)\n",
		       a->no, a->subno);
	  xfree (a->d.buf);
	  xfree (a->real_fname);
	  memcpy (a, b, sizeof *a);
	  xfree (b);
	  print_chain (a);
	}
      else
	a->filter_eof = 0;	/* for the top level filter */
      return -1;		/* return one(!) EOF */
    }

  if (a->d.len == 0 && a->error)
    /* The last time we tried to read from this filter, we got an
       error.  We couldn't return the error, because there was
       buffered data.  Since there is no longer any buffered data,
       return the error.  */
    {
      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: pending error (%s) returned\n",
		   a->no, a->subno, gpg_strerror (a->error));
      return -1;
    }

  if (a->filter && ! a->filter_eof && ! a->error)
    /* We have a filter function and the last time we tried to read we
       didn't get an EOF or an error.  Try to fill the buffer.  */
    {
      /* Be careful to account for any buffered data.  */
      len = a->d.size - a->d.len;

      if (a->e_d.preferred && a->d.len < IOBUF_ZEROCOPY_THRESHOLD_SIZE
	  && (IOBUF_ZEROCOPY_THRESHOLD_SIZE - a->d.len) < len)
	{
	  if (DBG_IOBUF)
	    log_debug ("iobuf-%d.%d: limit buffering as external drain is "
			"preferred\n",  a->no, a->subno);
	  len = IOBUF_ZEROCOPY_THRESHOLD_SIZE - a->d.len;
	}

      if (len == 0)
	/* There is no space for more data.  Don't bother calling
	   A->FILTER.  */
	rc = 0;
      else
      {
	/* If no buffered data and drain buffer has been setup, and drain
	 * buffer is largish, read data directly to drain buffer. */
	if (a->d.len == 0
	    && a->e_d.buf
	    && a->e_d.len >= IOBUF_ZEROCOPY_THRESHOLD_SIZE)
	  {
	    len = a->e_d.len;

	    if (DBG_IOBUF)
	      log_debug ("iobuf-%d.%d: underflow: A->FILTER (%lu bytes, to external drain)\n",
			 a->no, a->subno, (ulong)len);

	    rc = a->filter (a->filter_ov, IOBUFCTRL_UNDERFLOW, a->chain,
			    a->e_d.buf, &len);
	    a->e_d.used = len;
	    len = 0;
	  }
	else
	  {
	    if (DBG_IOBUF)
	      log_debug ("iobuf-%d.%d: underflow: A->FILTER (%lu bytes)\n",
			 a->no, a->subno, (ulong)len);

	    rc = a->filter (a->filter_ov, IOBUFCTRL_UNDERFLOW, a->chain,
			    &a->d.buf[a->d.len], &len);
	  }
      }
      a->d.len += len;

      if (DBG_IOBUF)
	log_debug ("iobuf-%d.%d: A->FILTER() returned rc=%d (%s), read %lu bytes%s\n",
		   a->no, a->subno,
		   rc, rc == 0 ? "ok" : rc == -1 ? "EOF" : gpg_strerror (rc),
		   (ulong)(a->e_d.used ? a->e_d.used : len),
		   a->e_d.used ? " (to external buffer)" : "");
/*  	    if( a->no == 1 ) */
/*                   log_hexdump ("     data:", a->d.buf, len); */

      if (rc == -1)
	/* EOF.  */
	{
	  size_t dummy_len = 0;

	  /* Tell the filter to free itself */
	  if ((rc = a->filter (a->filter_ov, IOBUFCTRL_FREE, a->chain,
			       NULL, &dummy_len)))
	    log_error ("IOBUFCTRL_FREE failed: %s\n", gpg_strerror (rc));

	  /* Free everything except for the internal buffer.  */
	  if (a->filter_ov && a->filter_ov_owner)
	    xfree (a->filter_ov);
	  a->filter_ov = NULL;
	  a->filter = NULL;
	  a->filter_eof = 1;

	  if (clear_pending_eof && a->d.len == 0 && a->e_d.used == 0
	      && a->chain)
	    /* We don't need to keep this filter around at all:

	         - we got an EOF
		 - we have no buffered data
		 - a filter follows this one.

	      Unlink this filter.  */
	    {
	      iobuf_t b = a->chain;
	      if (DBG_IOBUF)
		log_debug ("iobuf-%d.%d: pop in underflow (nothing buffered, got EOF)\n",
			   a->no, a->subno);
	      xfree (a->d.buf);
	      xfree (a->real_fname);
	      memcpy (a, b, sizeof *a);
	      xfree (b);

	      print_chain (a);

	      return -1;
	    }
	  else if (a->d.len == 0 && a->e_d.used == 0)
	    /* We can't unlink this filter (it is the only one in the
	       pipeline), but we can immediately return EOF.  */
	    return -1;
	}
      else if (rc)
	/* Record the error.  */
	{
	  a->error = rc;

	  if (a->d.len == 0 && a->e_d.used == 0)
	    /* There is no buffered data.  Immediately return EOF.  */
	    return -1;
	}
    }

  log_assert (a->d.start <= a->d.len);
  if (a->e_d.used > 0)
    return 0;
  if (a->d.start < a->d.len)
    return a->d.buf[a->d.start++];

  /* EOF.  */
  return -1;
}


static int
filter_flush (iobuf_t a)
{
  int external_used = 0;
  byte *src_buf;
  size_t src_len;
  size_t len;
  int rc;

  a->e_d.used = 0;

  if (a->use == IOBUF_OUTPUT_TEMP)
    {				/* increase the temp buffer */
      size_t newsize = a->d.size + iobuf_buffer_size;

      if (DBG_IOBUF)
	log_debug ("increasing temp iobuf from %lu to %lu\n",
		   (ulong) a->d.size, (ulong) newsize);

      a->d.buf = xrealloc (a->d.buf, newsize);
      a->d.size = newsize;
      return 0;
    }
  else if (a->use != IOBUF_OUTPUT)
    log_bug ("flush on non-output iobuf\n");
  else if (!a->filter)
    log_bug ("filter_flush: no filter\n");

  if (a->d.len == 0 && a->e_d.buf && a->e_d.len > 0)
    {
      src_buf = a->e_d.buf;
      src_len = a->e_d.len;
      external_used = 1;
    }
  else
    {
      src_buf = a->d.buf;
      src_len = a->d.len;
      external_used = 0;
    }

  len = src_len;
  rc = a->filter (a->filter_ov, IOBUFCTRL_FLUSH, a->chain, src_buf, &len);
  if (!rc && len != src_len)
    {
      log_info ("filter_flush did not write all!\n");
      rc = GPG_ERR_INTERNAL;
    }
  else if (rc)
    a->error = rc;
  a->d.len = 0;
  if (external_used)
    a->e_d.used = len;

  return rc;
}


int
iobuf_readbyte (iobuf_t a)
{
  int c;

  if (a->use == IOBUF_OUTPUT || a->use == IOBUF_OUTPUT_TEMP)
    {
      log_bug ("iobuf_readbyte called on a non-INPUT pipeline!\n");
      return -1;
    }

  log_assert (a->d.start <= a->d.len);

  if (a->nlimit && a->nbytes >= a->nlimit)
    return -1;			/* forced EOF */

  if (a->d.start < a->d.len)
    {
      c = a->d.buf[a->d.start++];
    }
  else if ((c = underflow (a, 1)) == -1)
    return -1;			/* EOF */

  log_assert (a->d.start <= a->d.len);

  /* Note: if underflow doesn't return EOF, then it returns the first
     byte that was read and advances a->d.start appropriately.  */

  a->nbytes++;
  return c;
}


int
iobuf_read (iobuf_t a, void *buffer, unsigned int buflen)
{
  unsigned char *buf = (unsigned char *)buffer;
  int c, n;

  if (a->use == IOBUF_OUTPUT || a->use == IOBUF_OUTPUT_TEMP)
    {
      log_bug ("iobuf_read called on a non-INPUT pipeline!\n");
      return -1;
    }

  if (a->nlimit)
    {
      /* Handle special cases. */
      for (n = 0; n < buflen; n++)
	{
	  if ((c = iobuf_readbyte (a)) == -1)
	    {
	      if (!n)
		return -1;	/* eof */
	      break;
	    }

	  if (buf)
	    {
	      *buf = c;
	      buf++;
	    }
	}
      return n;
    }

  a->e_d.buf = NULL;
  a->e_d.len = 0;

  /* Hint for how full to fill iobuf internal drain buffer. */
  a->e_d.preferred = (a->use != IOBUF_INPUT_TEMP)
    && (buf && buflen >= IOBUF_ZEROCOPY_THRESHOLD_SIZE);

  n = 0;
  do
    {
      if (n < buflen && a->d.start < a->d.len)
	/* Drain the buffer.  */
	{
	  unsigned size = a->d.len - a->d.start;
	  if (size > buflen - n)
	    size = buflen - n;
	  if (buf)
	    memcpy (buf, a->d.buf + a->d.start, size);
	  n += size;
	  a->d.start += size;
	  if (buf)
	    buf += size;
	}
      if (n < buflen)
	/* Draining the internal buffer didn't fill BUFFER.  Call
	   underflow to read more data into the filter's internal
	   buffer.  */
	{
	  if (a->use != IOBUF_INPUT_TEMP && buf && n < buflen)
	    {
	      /* Setup external drain buffer for faster moving of data
	       * (avoid memcpy). */
	      a->e_d.buf = buf;
	      a->e_d.len = (buflen - n) / IOBUF_ZEROCOPY_THRESHOLD_SIZE
			    * IOBUF_ZEROCOPY_THRESHOLD_SIZE;
	      if (a->e_d.len == 0)
		a->e_d.buf = NULL;
	      if (a->e_d.buf && DBG_IOBUF)
		log_debug ("iobuf-%d.%d: reading to external buffer, %lu bytes\n",
			   a->no, a->subno, (ulong)a->e_d.len);
	    }

	  if ((c = underflow (a, 1)) == -1)
	    /* EOF.  If we managed to read something, don't return EOF
	       now.  */
	    {
	      a->e_d.buf = NULL;
	      a->e_d.len = 0;
	      a->nbytes += n;
	      return n ? n : -1 /*EOF*/;
	    }

	  if (a->e_d.buf && a->e_d.used > 0)
	    {
	      /* Drain buffer was used, 'c' only contains return code
	       * 0 or -1. */
	      n += a->e_d.used;
	      buf += a->e_d.used;
	    }
	  else
	    {
	      if (buf)
		*buf++ = c;
	      n++;
	    }

	  a->e_d.buf = NULL;
	  a->e_d.len = 0;
	}
    }
  while (n < buflen);
  a->nbytes += n;
  return n;
}



int
iobuf_peek (iobuf_t a, byte * buf, unsigned buflen)
{
  int n = 0;

  log_assert (buflen > 0);
  log_assert (a->use == IOBUF_INPUT || a->use == IOBUF_INPUT_TEMP);

  if (buflen > a->d.size)
    /* We can't peek more than we can buffer.  */
    buflen = a->d.size;

  /* Try to fill the internal buffer with enough data to satisfy the
     request.  */
  while (buflen > a->d.len - a->d.start)
    {
      if (underflow_target (a, 0, buflen) == -1)
	/* EOF.  We can't read any more.  */
	break;

      /* Underflow consumes the first character (it's the return
	 value).  unget() it by resetting the "file position".  */
      log_assert (a->d.start == 1);
      a->d.start = 0;
    }

  n = a->d.len - a->d.start;
  if (n > buflen)
    n = buflen;

  if (n == 0)
    /* EOF.  */
    return -1;

  memcpy (buf, &a->d.buf[a->d.start], n);

  return n;
}




int
iobuf_writebyte (iobuf_t a, unsigned int c)
{
  int rc;

  if (a->use == IOBUF_INPUT || a->use == IOBUF_INPUT_TEMP)
    {
      log_bug ("iobuf_writebyte called on an input pipeline!\n");
      return -1;
    }

  if (a->d.len == a->d.size)
    if ((rc=filter_flush (a)))
      return rc;

  log_assert (a->d.len < a->d.size);
  a->d.buf[a->d.len++] = c;
  return 0;
}


int
iobuf_write (iobuf_t a, const void *buffer, unsigned int buflen)
{
  const unsigned char *buf = (const unsigned char *)buffer;
  int rc;

  if (a->use == IOBUF_INPUT || a->use == IOBUF_INPUT_TEMP)
    {
      log_bug ("iobuf_write called on an input pipeline!\n");
      return -1;
    }

  a->e_d.buf = NULL;
  a->e_d.len = 0;

  /* Hint for how full to fill iobuf internal drain buffer. */
  a->e_d.preferred = (a->use != IOBUF_OUTPUT_TEMP)
    && (buflen >= IOBUF_ZEROCOPY_THRESHOLD_SIZE);

  do
    {
      if ((a->use != IOBUF_OUTPUT_TEMP)
	  && a->d.len == 0 && buflen >= IOBUF_ZEROCOPY_THRESHOLD_SIZE)
	{
	  /* Setup external drain buffer for faster moving of data
	    * (avoid memcpy). */
	  a->e_d.buf = (byte *)buf;
	  a->e_d.len = buflen / IOBUF_ZEROCOPY_THRESHOLD_SIZE
			* IOBUF_ZEROCOPY_THRESHOLD_SIZE;
	  if (a->e_d.len == 0)
	    a->e_d.buf = NULL;
	  if (a->e_d.buf && DBG_IOBUF)
	    log_debug ("iobuf-%d.%d: writing from external buffer, %lu bytes\n",
			a->no, a->subno, (ulong)a->e_d.len);
	}

      if (a->e_d.buf == NULL && buflen && a->d.len < a->d.size)
	{
	  unsigned size;

	  if (a->e_d.preferred && a->d.len < IOBUF_ZEROCOPY_THRESHOLD_SIZE)
	    size = IOBUF_ZEROCOPY_THRESHOLD_SIZE - a->d.len;
	  else
	    size = a->d.size - a->d.len;

	  if (size > buflen)
	    size = buflen;
	  memcpy (a->d.buf + a->d.len, buf, size);
	  buflen -= size;
	  buf += size;
	  a->d.len += size;
	}

      if (buflen)
	{
	  rc = filter_flush (a);
          if (rc)
	    {
	      a->e_d.buf = NULL;
	      a->e_d.len = 0;
	      return rc;
	    }
	}

      if (a->e_d.buf && a->e_d.used > 0)
	{
	  buf += a->e_d.used;
	  buflen -= a->e_d.used;
	}

      a->e_d.buf = NULL;
      a->e_d.len = 0;
    }
  while (buflen);
  return 0;
}


int
iobuf_writestr (iobuf_t a, const char *buf)
{
  if (a->use == IOBUF_INPUT || a->use == IOBUF_INPUT_TEMP)
    {
      log_bug ("iobuf_writestr called on an input pipeline!\n");
      return -1;
    }

  return iobuf_write (a, buf, strlen (buf));
}



int
iobuf_write_temp (iobuf_t dest, iobuf_t source)
{
  log_assert (source->use == IOBUF_OUTPUT || source->use == IOBUF_OUTPUT_TEMP);
  log_assert (dest->use == IOBUF_OUTPUT || dest->use == IOBUF_OUTPUT_TEMP);

  iobuf_flush_temp (source);
  return iobuf_write (dest, source->d.buf, source->d.len);
}

size_t
iobuf_temp_to_buffer (iobuf_t a, byte * buffer, size_t buflen)
{
  byte desc[MAX_IOBUF_DESC];
  size_t n;

  while (1)
    {
      int rc = filter_flush (a);
      if (rc)
	log_bug ("Flushing iobuf %d.%d (%s) from iobuf_temp_to_buffer failed.  Ignoring.\n",
		 a->no, a->subno, iobuf_desc (a, desc));
      if (! a->chain)
	break;
      a = a->chain;
    }

  n = a->d.len;
  if (n > buflen)
    n = buflen;
  memcpy (buffer, a->d.buf, n);
  return n;
}

/* Copies the data from the input iobuf SOURCE to the output iobuf
   DEST until either an error is encountered or EOF is reached.
   Returns the number of bytes copies or (size_t)(-1) on error.  */
size_t
iobuf_copy (iobuf_t dest, iobuf_t source)
{
  char *temp;
  size_t temp_size;
  size_t nread;
  size_t nwrote = 0;
  size_t max_read = 0;
  int err;

  log_assert (source->use == IOBUF_INPUT || source->use == IOBUF_INPUT_TEMP);
  log_assert (dest->use == IOBUF_OUTPUT || source->use == IOBUF_OUTPUT_TEMP);

  if (iobuf_error (dest))
    return (size_t)(-1);

  /* Use iobuf buffer size for temporary buffer. */
  temp_size = iobuf_set_buffer_size(0) * 1024;

  temp = xmalloc (temp_size);
  while (1)
    {
      nread = iobuf_read (source, temp, temp_size);
      if (nread == -1)
        /* EOF.  */
        break;

      if (nread > max_read)
        max_read = nread;

      err = iobuf_write (dest, temp, nread);
      if (err)
        break;
      nwrote += nread;
    }

  /* Burn the buffer.  */
  if (max_read)
    wipememory (temp, max_read);
  xfree (temp);

  return nwrote;
}


void
iobuf_flush_temp (iobuf_t temp)
{
  if (temp->use == IOBUF_INPUT || temp->use == IOBUF_INPUT_TEMP)
    log_bug ("iobuf_flush_temp called on an input pipeline!\n");
  while (temp->chain)
    iobuf_pop_filter (temp, temp->filter, NULL);
}


void
iobuf_set_limit (iobuf_t a, off_t nlimit)
{
  if (nlimit)
    a->nofast = 1;
  else
    a->nofast = 0;
  a->nlimit = nlimit;
  a->ntotal += a->nbytes;
  a->nbytes = 0;
}


/* Return the length of the file behind A.  If there is no file, return 0. */
uint64_t
iobuf_get_filelength (iobuf_t a)
{
  /* Hmmm: file_filter may have already been removed */
  for ( ; a->chain; a = a->chain )
    ;

  if (a->filter != file_filter)
    return 0;

  {
    file_filter_ctx_t *b = a->filter_ov;
    gnupg_fd_t fp = b->fp;

#if defined(HAVE_W32_SYSTEM)
    LARGE_INTEGER exsize;

    if (GetFileSizeEx (fp, &exsize))
      return exsize.QuadPart;
    log_error ("GetFileSize for handle %p failed: %s\n",
	       fp, w32_strerror (-1));
#else /*!HAVE_W32_SYSTEM*/
    struct stat st;

    if ( !fstat (fp, &st) )
      return st.st_size;
    log_error("fstat() failed: %s\n", strerror(errno) );
#endif /*!HAVE_W32_SYSTEM*/
  }

  return 0;
}


gnupg_fd_t
iobuf_get_fd (iobuf_t a)
{
  for (; a->chain; a = a->chain)
    ;

  if (a->filter != file_filter)
    return GNUPG_INVALID_FD;

  {
    file_filter_ctx_t *b = a->filter_ov;
    gnupg_fd_t fp = b->fp;

    return fp;
  }
}


off_t
iobuf_tell (iobuf_t a)
{
  return a->ntotal + a->nbytes;
}


#if !defined(HAVE_FSEEKO) && !defined(fseeko)

#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif
#ifndef LONG_MAX
# define LONG_MAX ((long) ((unsigned long) -1 >> 1))
#endif
#ifndef LONG_MIN
# define LONG_MIN (-1 - LONG_MAX)
#endif

/****************
 * A substitute for fseeko, for hosts that don't have it.
 */
static int
fseeko (FILE * stream, off_t newpos, int whence)
{
  while (newpos != (long) newpos)
    {
      long pos = newpos < 0 ? LONG_MIN : LONG_MAX;
      if (fseek (stream, pos, whence) != 0)
	return -1;
      newpos -= pos;
      whence = SEEK_CUR;
    }
  return fseek (stream, (long) newpos, whence);
}
#endif

int
iobuf_seek (iobuf_t a, off_t newpos)
{
  file_filter_ctx_t *b = NULL;

  if (a->use == IOBUF_OUTPUT || a->use == IOBUF_INPUT)
    {
      /* Find the last filter in the pipeline.  */
      for (; a->chain; a = a->chain)
	;

      if (a->filter != file_filter)
	return -1;

      b = a->filter_ov;

#ifdef HAVE_W32_SYSTEM
      if (SetFilePointer (b->fp, newpos, NULL, FILE_BEGIN) == 0xffffffff)
	{
	  log_error ("SetFilePointer failed on handle %p: ec=%d\n",
		     b->fp, (int) GetLastError ());
	  return -1;
	}
#else
      if (lseek (b->fp, newpos, SEEK_SET) == (off_t) - 1)
	{
	  log_error ("can't lseek: %s\n", strerror (errno));
	  return -1;
	}
#endif
      /* Discard the buffer it is not a temp stream.  */
      a->d.len = 0;
    }
  a->d.start = 0;
  a->nbytes = 0;
  a->nlimit = 0;
  a->nofast = 0;
  a->ntotal = newpos;
  a->error = 0;

  /* It is impossible for A->CHAIN to be non-NULL.  If A is an INPUT
     or OUTPUT buffer, then we find the last filter, which is defined
     as A->CHAIN being NULL.  If A is a TEMP filter, then A must be
     the only filter in the pipe: when iobuf_push_filter adds a filter
     to the front of a pipeline, it sets the new filter to be an
     OUTPUT filter if the pipeline is an OUTPUT or TEMP pipeline and
     to be an INPUT filter if the pipeline is an INPUT pipeline.
     Thus, only the last filter in a TEMP pipeline can be a */

  /* remove filters, but the last */
  if (a->chain)
    log_debug ("iobuf_pop_filter called in iobuf_seek - please report\n");
  while (a->chain)
    iobuf_pop_filter (a, a->filter, NULL);

  return 0;
}


const char *
iobuf_get_real_fname (iobuf_t a)
{
  if (a->real_fname)
    return a->real_fname;

  /* the old solution */
  for (; a; a = a->chain)
    if (!a->chain && a->filter == file_filter)
      {
	file_filter_ctx_t *b = a->filter_ov;
	return b->print_only_name ? NULL : b->fname;
      }

  return NULL;
}

const char *
iobuf_get_fname (iobuf_t a)
{
  for (; a; a = a->chain)
    if (!a->chain && a->filter == file_filter)
      {
	file_filter_ctx_t *b = a->filter_ov;
	return b->fname;
      }
  return NULL;
}

const char *
iobuf_get_fname_nonnull (iobuf_t a)
{
  const char *fname;

  fname = iobuf_get_fname (a);
  return fname? fname : "[?]";
}


/****************
 * Enable or disable partial body length mode (RFC 4880 4.2.2.4).
 *
 * If LEN is 0, this disables partial block mode by popping the
 * partial body length filter, which must be the most recently
 * added filter.
 *
 * If LEN is non-zero, it pushes a partial body length filter.  If
 * this is a read filter, LEN must be the length byte from the first
 * chunk and A should be position just after this first partial body
 * length header.
 */
void
iobuf_set_partial_body_length_mode (iobuf_t a, size_t len)
{
  if (!len)
    /* Disable partial body length mode.  */
    {
      if (a->use == IOBUF_INPUT)
	log_debug ("iobuf_pop_filter called in set_partial_block_mode"
		   " - please report\n");

      log_assert (a->filter == block_filter);
      iobuf_pop_filter (a, block_filter, NULL);
    }
  else
    /* Enabled partial body length mode.  */
    {
      block_filter_ctx_t *ctx = xcalloc (1, sizeof *ctx);
      ctx->use = a->use;
      ctx->partial = 1;
      ctx->size = 0;
      ctx->first_c = len;
      iobuf_push_filter (a, block_filter, ctx);
    }
}



unsigned int
iobuf_read_line (iobuf_t a, byte ** addr_of_buffer,
		 unsigned *length_of_buffer, unsigned *max_length)
{
  int c;
  char *buffer = (char *)*addr_of_buffer;
  unsigned length = *length_of_buffer;
  unsigned nbytes = 0;
  unsigned maxlen = *max_length;
  char *p;

  /* The code assumes that we have space for at least a newline and a
     NUL character in the buffer.  This requires at least 2 bytes.  We
     don't complicate the code by handling the stupid corner case, but
     simply assert that it can't happen.  */
  log_assert (!buffer || length >= 2 || maxlen >= 2);

  if (!buffer || length <= 1)
    /* must allocate a new buffer */
    {
      length = 256 <= maxlen ? 256 : maxlen;
      buffer = xrealloc (buffer, length);
      *addr_of_buffer = (unsigned char *)buffer;
      *length_of_buffer = length;
    }

  p = buffer;
  while (1)
    {
      if (!a->nofast && a->d.start < a->d.len && nbytes < length - 1)
	/* Fast path for finding '\n' by using standard C library's optimized
	   memchr.  */
	{
	  unsigned size = a->d.len - a->d.start;
	  byte *newline_pos;

	  if (size > length - 1 - nbytes)
	    size = length - 1 - nbytes;

	  newline_pos = memchr (a->d.buf + a->d.start, '\n', size);
	  if (newline_pos)
	    {
	      /* Found newline, copy buffer and return. */
	      size = (newline_pos - (a->d.buf + a->d.start)) + 1;
	      memcpy (p, a->d.buf + a->d.start, size);
	      p += size;
	      nbytes += size;
	      a->d.start += size;
	      a->nbytes += size;
	      break;
	    }
	  else
	    {
	      /* No newline, copy buffer and continue. */
	      memcpy (p, a->d.buf + a->d.start, size);
	      p += size;
	      nbytes += size;
	      a->d.start += size;
	      a->nbytes += size;
	    }
	}
      else
	{
	  c = iobuf_readbyte (a);
	  if (c == -1)
	    break;
	  *p++ = c;
	  nbytes++;
	  if (c == '\n')
	    break;
	}

      if (nbytes == length - 1)
	/* We don't have enough space to add a \n and a \0.  Increase
	   the buffer size.  */
	{
	  if (length == maxlen)
	    /* We reached the buffer's size limit!  */
	    {
	      /* Skip the rest of the line.  */
	      while ((c = iobuf_get (a)) != -1 && c != '\n')
		;

	      /* p is pointing at the last byte in the buffer.  We
		 always terminate the line with "\n\0" so overwrite
		 the previous byte with a \n.  */
	      log_assert (p > buffer);
	      p[-1] = '\n';

	      /* Indicate truncation.  */
	      *max_length = 0;
	      break;
	    }

	  length += length < 1024 ? 256 : 1024;
	  if (length > maxlen)
	    length = maxlen;

	  buffer = xrealloc (buffer, length);
	  *addr_of_buffer = (unsigned char *)buffer;
	  *length_of_buffer = length;
	  p = buffer + nbytes;
	}
    }
  /* Add the terminating NUL.  */
  *p = 0;

  /* Return the number of characters written to the buffer including
     the newline, but not including the terminating NUL.  */
  return nbytes;
}


void
iobuf_skip_rest (iobuf_t a, unsigned long n, int partial)
{
  if ( partial )
    {
      for (;;)
        {
          if (a->nofast || a->d.start >= a->d.len)
            {
              if (iobuf_readbyte (a) == -1)
                {
                  break;
                }
	    }
          else
            {
              unsigned long count = a->d.len - a->d.start;
              a->nbytes += count;
              a->d.start = a->d.len;
	    }
	}
    }
  else
    {
      unsigned long remaining = n;
      while (remaining > 0)
        {
          if (a->nofast || a->d.start >= a->d.len)
            {
              if (iobuf_readbyte (a) == -1)
                {
                  break;
		}
              --remaining;
	    }
          else
            {
              unsigned long count = a->d.len - a->d.start;
              if (count > remaining)
                {
                  count = remaining;
		}
              a->nbytes += count;
              a->d.start += count;
              remaining -= count;
	    }
	}
    }
}


/* Check whether (BUF,LEN) is valid header for an OpenPGP compressed
 * packet.  LEN should be at least 6.  */
static int
is_openpgp_compressed_packet (const unsigned char *buf, size_t len)
{
  int c, ctb, pkttype;
  int lenbytes;

  ctb = *buf++; len--;
  if (!(ctb & 0x80))
    return 0; /* Invalid packet.  */

  if ((ctb & 0x40)) /* New style (OpenPGP) CTB.  */
    {
      pkttype = (ctb & 0x3f);
      if (!len)
        return 0; /* Expected first length octet missing.  */
      c = *buf++; len--;
      if (c < 192)
        ;
      else if (c < 224)
        {
          if (!len)
            return 0; /* Expected second length octet missing. */
        }
      else if (c == 255)
        {
          if (len < 4)
            return 0; /* Expected length octets missing */
        }
    }
  else /* Old style CTB.  */
    {
      pkttype = (ctb>>2)&0xf;
      lenbytes = ((ctb&3)==3)? 0 : (1<<(ctb & 3));
      if (len < lenbytes)
        return 0; /* Not enough length bytes.  */
    }

  return (pkttype == 8);
}


/*
 * Check if the file is compressed, by peeking the iobuf.  You need to
 * pass the iobuf with INP.  Returns true if the buffer seems to be
 * compressed.
 */
int
is_file_compressed (iobuf_t inp)
{
  int i;
  char buf[32];
  int buflen;

  struct magic_compress_s
  {
    byte len;
    byte extchk;
    byte magic[5];
  } magic[] =
      {
       { 3, 0, { 0x42, 0x5a, 0x68, 0x00 } }, /* bzip2 */
       { 3, 0, { 0x1f, 0x8b, 0x08, 0x00 } }, /* gzip */
       { 4, 0, { 0x50, 0x4b, 0x03, 0x04 } }, /* (pk)zip */
       { 5, 0, { '%', 'P', 'D', 'F', '-'} }, /* PDF */
       { 4, 1, { 0xff, 0xd8, 0xff, 0xe0 } }, /* Maybe JFIF */
       { 5, 2, { 0x89, 'P','N','G', 0x0d} }  /* Likely PNG */
  };

  if (!inp)
    return 0;

  for ( ; inp->chain; inp = inp->chain )
    ;

  buflen = iobuf_ioctl (inp, IOBUF_IOCTL_PEEK, sizeof buf, buf);
  if (buflen < 0)
    {
      buflen = 0;
      log_debug ("peeking at input failed\n");
    }

  if ( buflen < 6 )
    {
      return 0;  /* Too short to check - assume uncompressed.  */
    }

  for ( i = 0; i < DIM (magic); i++ )
    {
      if (!memcmp( buf, magic[i].magic, magic[i].len))
        {
          switch (magic[i].extchk)
            {
            case 0:
              return 1; /* Is compressed.  */
            case 1:
              if (buflen > 11 && !memcmp (buf + 6, "JFIF", 5))
                return 1; /* JFIF: this likely a compressed JPEG.  */
              break;
            case 2:
              if (buflen > 8
                  && buf[5] == 0x0a && buf[6] == 0x1a && buf[7] == 0x0a)
                return 1; /* This is a PNG.  */
              break;
            default:
              break;
            }
        }
    }

  if (buflen >= 6 && is_openpgp_compressed_packet (buf, buflen))
    {
      return 1; /* Already compressed.  */
    }

  return 0;  /* Not detected as compressed.  */
}
