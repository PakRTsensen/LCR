/* compress.c - compress filter
 * Copyright (C) 1998, 1999, 2000, 2001, 2002,
 *               2003, 2006, 2010 Free Software Foundation, Inc.
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

/* Note that the code in compress-bz2.c is nearly identical to the
   code here, so if you fix a bug here, look there to see if a
   matching bug needs to be fixed.  I tried to have one set of
   functions that could do ZIP, ZLIB, and BZIP2, but it became
   dangerously unreadable with #ifdefs and if(algo) -dshaw */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_ZIP
# include <zlib.h>
#endif

#include "lcr.h"
#include "../common/util.h"
#include "packet.h"
#include "filter.h"
#include "main.h"
#include "options.h"


#ifdef __riscos__
#define BYTEF_CAST(a) ((Bytef *)(a))
#else
#define BYTEF_CAST(a) (a)
#endif



int compress_filter_bz2( void *opaque, int control,
			 IOBUF a, byte *buf, size_t *ret_len);

#ifdef HAVE_ZIP
static void
init_compress( compress_filter_context_t *zfx, z_stream *zs )
{
    int rc;
    int level;

    if( opt.compress_level >= 1 && opt.compress_level <= 9 )
	level = opt.compress_level;
    else if( opt.compress_level == -1 )
	level = Z_DEFAULT_COMPRESSION;
    else {
	log_error("invalid compression level; using default level\n");
	level = Z_DEFAULT_COMPRESSION;
    }

    if( (rc = zfx->algo == 1? deflateInit2( zs, level, Z_DEFLATED,
					    -13, 8, Z_DEFAULT_STRATEGY)
			    : deflateInit( zs, level )
			    ) != Z_OK ) {
	log_error ("zlib problem: %s\n", zs->msg? zs->msg :
			       rc == Z_MEM_ERROR ? "out of core" :
			       rc == Z_VERSION_ERROR ? "invalid lib version" :
						       "unknown error" );
        write_status_error ("zlib.init", gpg_error (GPG_ERR_INTERNAL));
        g10_exit (2);
    }

    zfx->outbufsize = 65536;
    zfx->outbuf = xmalloc( zfx->outbufsize );
}

static int
do_compress( compress_filter_context_t *zfx, z_stream *zs, int flush, IOBUF a )
{
    int rc;
    int zrc;
    unsigned n;

    if (flush == Z_NO_FLUSH && zs->avail_in == 0)
      return 0;

    do {
	zs->next_out = BYTEF_CAST (zfx->outbuf);
	zs->avail_out = zfx->outbufsize;
	if( DBG_FILTER )
	    log_debug("enter deflate: avail_in=%u, avail_out=%u, flush=%d\n",
		    (unsigned)zs->avail_in, (unsigned)zs->avail_out, flush );
	zrc = deflate( zs, flush );
	if( zrc == Z_STREAM_END && flush == Z_FINISH )
	    ;
	else if( zrc != Z_OK ) {
	    if( zs->msg )
		log_error ("zlib deflate problem: %s\n", zs->msg );
	    else
		log_error ("zlib deflate problem: rc=%d\n", zrc );
            write_status_error ("zlib.deflate", gpg_error (GPG_ERR_INTERNAL));
            g10_exit (2);
	}
	n = zfx->outbufsize - zs->avail_out;
	if( DBG_FILTER )
	    log_debug("leave deflate: "
		      "avail_in=%u, avail_out=%u, n=%u, zrc=%d\n",
		(unsigned)zs->avail_in, (unsigned)zs->avail_out,
					       (unsigned)n, zrc );

	if( (rc=iobuf_write( a, zfx->outbuf, n )) ) {
	    log_error ("deflate: iobuf_write failed\n");
	    return rc;
	}
    } while( zs->avail_in || (flush == Z_FINISH && zrc != Z_STREAM_END) );
    return 0;
}

static void
init_uncompress( compress_filter_context_t *zfx, z_stream *zs )
{
    int rc;

    /****************
     * PGP uses a windowsize of 13 bits. Using a negative value for
     * it forces zlib not to expect a zlib header.  This is a
     * undocumented feature Peter Gutmann told me about.
     *
     * We must use 15 bits for the inflator because CryptoEx uses 15
     * bits thus the output would get scrambled w/o error indication
     * if we would use 13 bits.  For the uncompressing this does not
     * matter at all.
     */
    if( (rc = zfx->algo == 1? inflateInit2( zs, -15)
			    : inflateInit( zs )) != Z_OK ) {
	log_error ("zlib problem: %s\n", zs->msg? zs->msg :
                   rc == Z_MEM_ERROR ? "out of core" :
                   rc == Z_VERSION_ERROR ? "invalid lib version" :
                                            "unknown error" );
        write_status_error ("zlib.init.un", gpg_error (GPG_ERR_INTERNAL));
        g10_exit (2);
    }

    zfx->inbufsize = 2048;
    zfx->inbuf = xmalloc( zfx->inbufsize );
    zs->avail_in = 0;
}

static int
do_uncompress( compress_filter_context_t *zfx, z_stream *zs,
	       IOBUF a, size_t *ret_len )
{
    int zrc;
    int rc = 0;
    int leave = 0;
    size_t n;
    int nread, count;
    int refill = !zs->avail_in;

    if( DBG_FILTER )
	log_debug("begin inflate: avail_in=%u, avail_out=%u, inbuf=%u\n",
		(unsigned)zs->avail_in, (unsigned)zs->avail_out,
		(unsigned)zfx->inbufsize );
    do {
	if( zs->avail_in < zfx->inbufsize && refill ) {
	    n = zs->avail_in;
	    if( !n )
            zs->next_in = BYTEF_CAST (zfx->inbuf);
	    count = zfx->inbufsize - n;
	    nread = iobuf_read( a, zfx->inbuf + n, count );
	    if( nread == -1 ) nread = 0;
	    n += nread;
	    /* Algo 1 has no zlib header which requires us to give
	     * inflate an extra dummy byte to read. To be on the safe
	     * side we allow for up to 4 ff bytes.  */
	    if( nread < count && zfx->algo == 1 && zfx->algo1hack < 4) {
		*(zfx->inbuf + n) = 0xFF;
		zfx->algo1hack++;
		n++;
                leave = 1;
	    }
	    zs->avail_in = n;
	}
	refill = 1;
	if( DBG_FILTER )
	    log_debug("enter inflate: avail_in=%u, avail_out=%u\n",
		    (unsigned)zs->avail_in, (unsigned)zs->avail_out);
	zrc = inflate ( zs, Z_SYNC_FLUSH );
	if( DBG_FILTER )
	    log_debug("leave inflate: avail_in=%u, avail_out=%u, zrc=%d\n",
		   (unsigned)zs->avail_in, (unsigned)zs->avail_out, zrc);
	if( zrc == Z_STREAM_END )
	    rc = -1; /* eof */
	else if( zrc != Z_OK && zrc != Z_BUF_ERROR ) {
	    if( zs->msg )
		log_error ("zlib inflate problem: %s\n", zs->msg );
	    else
		log_error ("zlib inflate problem: rc=%d\n", zrc );
            write_status_error ("zlib.inflate", gpg_error (GPG_ERR_BAD_DATA));
            g10_exit (2);
	}
    } while (zs->avail_out && zrc != Z_STREAM_END && zrc != Z_BUF_ERROR
             && !leave);

    *ret_len = zfx->outbufsize - zs->avail_out;
    if( DBG_FILTER )
	log_debug("do_uncompress: returning %u bytes (%u ignored)\n",
                  (unsigned int)*ret_len, (unsigned int)zs->avail_in );
    return rc;
}

static int
compress_filter( void *opaque, int control,
		 IOBUF a, byte *buf, size_t *ret_len)
{
    size_t size = *ret_len;
    compress_filter_context_t *zfx = opaque;
    z_stream *zs = zfx->opaque;
    int rc=0;

    if( control == IOBUFCTRL_UNDERFLOW ) {
	if( !zfx->status ) {
	    zs = zfx->opaque = xmalloc_clear( sizeof *zs );
	    init_uncompress( zfx, zs );
	    zfx->status = 1;
	}

	zs->next_out = BYTEF_CAST (buf);
	zs->avail_out = size;
	zfx->outbufsize = size; /* needed only for calculation */
	rc = do_uncompress( zfx, zs, a, ret_len );
    }
    else if( control == IOBUFCTRL_FLUSH ) {
	if( !zfx->status ) {
	    PACKET pkt;
	    PKT_compressed cd;
	    if(zfx->algo != COMPRESS_ALGO_ZIP
	       && zfx->algo != COMPRESS_ALGO_ZLIB)
	      BUG();
	    memset( &cd, 0, sizeof cd );
	    cd.len = 0;
	    cd.algorithm = zfx->algo;
            /* Fixme: We should force a new CTB here:
               cd.new_ctb = zfx->new_ctb;
            */
	    init_packet( &pkt );
	    pkt.pkttype = PKT_COMPRESSED;
	    pkt.pkt.compressed = &cd;
	    if( build_packet( a, &pkt ))
		log_bug("build_packet(PKT_COMPRESSED) failed\n");
	    zs = zfx->opaque = xmalloc_clear( sizeof *zs );
	    init_compress( zfx, zs );
	    zfx->status = 2;
	}

	zs->next_in = BYTEF_CAST (buf);
	zs->avail_in = size;
	rc = do_compress( zfx, zs, Z_NO_FLUSH, a );
    }
    else if( control == IOBUFCTRL_FREE ) {
	if( zfx->status == 1 ) {
	    inflateEnd(zs);
	    xfree(zs);
	    zfx->opaque = NULL;
	    xfree(zfx->outbuf); zfx->outbuf = NULL;
	}
	else if( zfx->status == 2 ) {
	    zs->next_in = BYTEF_CAST (buf);
	    zs->avail_in = 0;
	    do_compress( zfx, zs, Z_FINISH, a );
	    deflateEnd(zs);
	    xfree(zs);
	    zfx->opaque = NULL;
	    xfree(zfx->outbuf); zfx->outbuf = NULL;
	}
        if (zfx->release)
          zfx->release (zfx);
    }
    else if( control == IOBUFCTRL_DESC )
        mem2str (buf, "compress_filter", *ret_len);
    return rc;
}
#endif /*HAVE_ZIP*/

static void
release_context (compress_filter_context_t *ctx)
{
  xfree(ctx->inbuf);
  ctx->inbuf = NULL;
  xfree(ctx->outbuf);
  ctx->outbuf = NULL;
  xfree (ctx);
}

/****************
 * Handle a compressed packet
 */
int
handle_compressed (ctrl_t ctrl, void *procctx, PKT_compressed *cd,
		   int (*callback)(IOBUF, void *), void *passthru )
{
    int rc;

    if(check_compress_algo(cd->algorithm))
      return GPG_ERR_COMPR_ALGO;
    if(cd->algorithm) {
        compress_filter_context_t *cfx;

        cfx = xmalloc_clear (sizeof *cfx);
        cfx->release = release_context;
        cfx->algo = cd->algorithm;
        if (push_compress_filter(cd->buf, cfx, cd->algorithm))
          xfree (cfx);
    }
    if( callback )
	rc = callback(cd->buf, passthru );
    else
      rc = proc_packets (ctrl,procctx, cd->buf);
    cd->buf = NULL;
    return rc;
}

gpg_error_t
push_compress_filter(IOBUF out,compress_filter_context_t *zfx,int algo)
{
  return push_compress_filter2(out,zfx,algo,0);
}


/* Push a compress filter and return 0 if that succeeded.  */
gpg_error_t
push_compress_filter2(IOBUF out,compress_filter_context_t *zfx,
		      int algo,int rel)
{
  gpg_error_t err = gpg_error (GPG_ERR_FALSE);

  if(algo>=0)
    zfx->algo=algo;
  else
    zfx->algo=DEFAULT_COMPRESS_ALGO;

  switch(zfx->algo)
    {
    case COMPRESS_ALGO_NONE:
      break;

#ifdef HAVE_ZIP
    case COMPRESS_ALGO_ZIP:
    case COMPRESS_ALGO_ZLIB:
      iobuf_push_filter2(out,compress_filter,zfx,rel);
      err = 0;
      break;
#endif

#ifdef HAVE_BZIP2
    case COMPRESS_ALGO_BZIP2:
      iobuf_push_filter2(out,compress_filter_bz2,zfx,rel);
      err = 0;
      break;
#endif

    default:
      BUG();
    }

  return err;
}
