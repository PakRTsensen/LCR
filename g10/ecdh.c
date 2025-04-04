/* ecdh.c - ECDH public key operations used in public key glue code
 *	Copyright (C) 2010, 2011 Free Software Foundation, Inc.
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
#include "../common/util.h"
#include "pkglue.h"
#include "main.h"
#include "options.h"

/* A table with the default KEK parameters used by GnuPG.  */
static const struct
{
  unsigned int qbits;
  int openpgp_hash_id;   /* KEK digest algorithm. */
  int openpgp_cipher_id; /* KEK cipher algorithm. */
} kek_params_table[] =
  /* Note: Must be sorted by ascending values for QBITS.  */
  {
    { 256, DIGEST_ALGO_SHA256, CIPHER_ALGO_AES    },
    { 384, DIGEST_ALGO_SHA384, CIPHER_ALGO_AES256 },

    /* Note: 528 is 521 rounded to the 8 bit boundary */
    { 528, DIGEST_ALGO_SHA512, CIPHER_ALGO_AES256 }
  };



/* Return KEK parameters as an opaque MPI The caller must free the
   returned value.  Returns NULL and sets ERRNO on error.  */
gcry_mpi_t
pk_ecdh_default_params (unsigned int qbits)
{
  byte kek_params[4] = {
    3, /* Number of bytes to follow. */
    1  /* Version for KDF+AESWRAP.   */
  };
  int i;

  /* Search for matching KEK parameter.  Defaults to the strongest
     possible choices.  Performance is not an issue here, only
     interoperability.  */
  for (i=0; i < DIM (kek_params_table); i++)
    {
      if (kek_params_table[i].qbits >= qbits
          || i+1 == DIM (kek_params_table))
        {
          kek_params[2] = kek_params_table[i].openpgp_hash_id;
          kek_params[3] = kek_params_table[i].openpgp_cipher_id;
          break;
        }
    }
  log_assert (i < DIM (kek_params_table));
  if (DBG_CRYPTO)
    log_printhex (kek_params, sizeof(kek_params), "ECDH KEK params are");

  return gcry_mpi_set_opaque_copy (NULL, kek_params, 4 * 8);
}


/* Extract xcomponent from the point SHARED.  POINT_NBYTES is the
   size to represent an EC point which is determined by the public
   key.  SECRET_X_SIZE is the size of x component to represent an
   integer which is determined by the curve. */
static gpg_error_t
extract_secret_x (byte **r_secret_x,
                  const char *shared, size_t nshared,
                  size_t point_nbytes, size_t secret_x_size)
{
  byte *secret_x;

  *r_secret_x = NULL;

  /* Extract X from the result.  It must be in the format of:
     04 || X || Y
     40 || X
     41 || X

     Since it may come with the prefix, the size of point is larger
     than or equals to the size of an integer X.  We also better check
     that the provided shared point is not larger than the size needed
     to represent the point.  */
  if (point_nbytes < secret_x_size)
    return gpg_error (GPG_ERR_BAD_DATA);
  if (point_nbytes < nshared)
    return gpg_error (GPG_ERR_BAD_DATA);

  /* Extract x component of the shared point: this is the actual
     shared secret. */
  secret_x = xtrymalloc_secure (point_nbytes);
  if (!secret_x)
    return gpg_error_from_syserror ();

  memcpy (secret_x, shared, nshared);

  /* Wrangle the provided point unless only the x-component w/o any
   * prefix was provided.  */
  if (nshared != secret_x_size)
    {
      /* Remove the prefix.  */
      if ((point_nbytes & 1))
        memmove (secret_x, secret_x+1, secret_x_size);

      /* Clear the rest of data.  */
      if (point_nbytes - secret_x_size)
        memset (secret_x+secret_x_size, 0, point_nbytes-secret_x_size);
    }

  if (DBG_CRYPTO)
    log_printhex (secret_x, secret_x_size, "ECDH shared secret X is:");

  *r_secret_x = secret_x;
  return 0;
}


/* Build KDF parameters */
/* RFC 6637 defines the KDF parameters and its encoding in Section
   8. EC DH Algorighm (ECDH).  Since it was written for v4 key, it
   said "20 octets representing a recipient encryption subkey or a
   master key fingerprint".  For v5 key, it is considered "adequate"
   (in terms of NIST SP 800 56A, see 5.8.2 FixedInfo) to use the first
   20 octets of its 32 octets fingerprint.  */
static gpg_error_t
build_kdf_params (unsigned char kdf_params[256], size_t *r_size,
                  gcry_mpi_t *pkey, const byte pk_fp[MAX_FINGERPRINT_LEN])
{
  IOBUF obuf;
  gpg_error_t err;

  *r_size = 0;

  obuf = iobuf_temp();
  if (!obuf)
    return gpg_error_from_syserror ();

  /* variable-length field 1, curve name OID */
  err = gpg_mpi_write_opaque_nohdr (obuf, pkey[0]);
  /* fixed-length field 2 */
  iobuf_put (obuf, PUBKEY_ALGO_ECDH);
  /* variable-length field 3, KDF params */
  err = (err ? err : gpg_mpi_write_opaque_nohdr (obuf, pkey[2]));
  /* fixed-length field 4 */
  iobuf_write (obuf, "Anonymous Sender    ", 20);
  /* fixed-length field 5, recipient fp (or first 20 octets of fp) */
  iobuf_write (obuf, pk_fp, 20);

  if (!err)
    *r_size = iobuf_temp_to_buffer (obuf, kdf_params, 256);

  iobuf_close (obuf);

  if (!err)
    {
      if (DBG_CRYPTO)
        log_printhex (kdf_params, *r_size, "ecdh KDF message params are:");
    }

  return err;
}


/* Derive KEK with KEK_SIZE into the memory at SECRET_X.  */
static gpg_error_t
derive_kek (size_t kek_size,
            int kdf_hash_algo,
            byte *secret_x, int secret_x_size,
            const unsigned char *kdf_params, size_t kdf_params_size)
{
  gpg_error_t err;
#if 0 /* GCRYPT_VERSION_NUMBER >= 0x010b00 */
  /*
   * Experimental: We will remove this if/endif-conditional
   * compilation when we update NEED_LIBGCRYPT_VERSION to 1.11.0.
   */
  gcry_kdf_hd_t hd;
  unsigned long param[1];

  param[0] = kek_size;
  err = gcry_kdf_open (&hd, GCRY_KDF_ONESTEP_KDF, kdf_hash_algo,
                       param, 1,
                       secret_x, secret_x_size, NULL, 0, NULL, 0,
                       kdf_params, kdf_params_size);
  if (!err)
    {
      gcry_kdf_compute (hd, NULL);
      gcry_kdf_final (hd, kek_size, secret_x);
      gcry_kdf_close (hd);
      /* Clean the tail before returning.  */
      memset (secret_x+kek_size, 0, secret_x_size - kek_size);
    }
#else
  gcry_md_hd_t h;

  log_assert( gcry_md_get_algo_dlen (kdf_hash_algo) >= 32 );

  err = gcry_md_open (&h, kdf_hash_algo, 0);
  if (err)
    {
      log_error ("gcry_md_open failed for kdf_hash_algo %d: %s",
                 kdf_hash_algo, gpg_strerror (err));
      return err;
    }
  gcry_md_write(h, "\x00\x00\x00\x01", 4);      /* counter = 1 */
  gcry_md_write(h, secret_x, secret_x_size);    /* x of the point X */
  gcry_md_write(h, kdf_params, kdf_params_size);      /* KDF parameters */
  gcry_md_final (h);
  memcpy (secret_x, gcry_md_read (h, kdf_hash_algo), kek_size);
  gcry_md_close (h);
  /* Clean the tail before returning.  */
  memset (secret_x+kek_size, 0, secret_x_size - kek_size);
#endif
  if (DBG_CRYPTO)
    log_printhex (secret_x, kek_size, "ecdh KEK is:");
  return err;
}


/* Prepare ECDH using SHARED, PK_FP fingerprint, and PKEY array.
   Returns the cipher handle in R_HD, which needs to be closed by
   the caller.  */
static gpg_error_t
prepare_ecdh_with_shared_point (const char *shared, size_t nshared,
                                const byte pk_fp[MAX_FINGERPRINT_LEN],
                                gcry_mpi_t *pkey, gcry_cipher_hd_t *r_hd)
{
  gpg_error_t err;
  byte *secret_x;
  int secret_x_size;
  unsigned int nbits;
  const unsigned char *kek_params;
  size_t kek_params_size;
  int kdf_hash_algo;
  int kdf_encr_algo;
  unsigned char kdf_params[256];
  size_t kdf_params_size;
  size_t kek_size;
  gcry_cipher_hd_t hd;

  *r_hd = NULL;

  if (!gcry_mpi_get_flag (pkey[2], GCRYMPI_FLAG_OPAQUE))
    return gpg_error (GPG_ERR_BUG);

  kek_params = gcry_mpi_get_opaque (pkey[2], &nbits);
  kek_params_size = (nbits+7)/8;

  if (DBG_CRYPTO)
    log_printhex (kek_params, kek_params_size, "ecdh KDF params:");

  /* Expect 4 bytes  03 01 hash_alg symm_alg.  */
  if (kek_params_size != 4 || kek_params[0] != 3 || kek_params[1] != 1)
    return gpg_error (GPG_ERR_BAD_PUBKEY);

  kdf_hash_algo = kek_params[2];
  kdf_encr_algo = kek_params[3];

  if (DBG_CRYPTO)
    log_debug ("ecdh KDF algorithms %s+%s with aeswrap\n",
               openpgp_md_algo_name (kdf_hash_algo),
               openpgp_cipher_algo_name (kdf_encr_algo));

  if (kdf_hash_algo != GCRY_MD_SHA256
      && kdf_hash_algo != GCRY_MD_SHA384
      && kdf_hash_algo != GCRY_MD_SHA512)
    return gpg_error (GPG_ERR_BAD_PUBKEY);

  if (kdf_encr_algo != CIPHER_ALGO_AES
      && kdf_encr_algo != CIPHER_ALGO_AES192
      && kdf_encr_algo != CIPHER_ALGO_AES256)
    return gpg_error (GPG_ERR_BAD_PUBKEY);

  kek_size = gcry_cipher_get_algo_keylen (kdf_encr_algo);
  if (kek_size > gcry_md_get_algo_dlen (kdf_hash_algo))
    return gpg_error (GPG_ERR_BAD_PUBKEY);

  /* Build kdf_params.  */
  err = build_kdf_params (kdf_params, &kdf_params_size, pkey, pk_fp);
  if (err)
    return err;

  nbits = pubkey_nbits (PUBKEY_ALGO_ECDH, pkey);
  if (!nbits)
    return gpg_error (GPG_ERR_TOO_SHORT);

  secret_x_size = (nbits+7)/8;
  if (kek_size > secret_x_size)
    return gpg_error (GPG_ERR_BAD_PUBKEY);

  err = extract_secret_x (&secret_x, shared, nshared,
                          /* pkey[1] is the public point */
                          (mpi_get_nbits (pkey[1])+7)/8,
                          secret_x_size);
  if (err)
    return err;

  /*** We have now the shared secret bytes in secret_x. ***/

  /* At this point we are done with PK encryption and the rest of the
   * function uses symmetric key encryption techniques to protect the
   * input DATA.  The following two sections will simply replace
   * current secret_x with a value derived from it.  This will become
   * a KEK.
   */

  /* Derive a KEK (key wrapping key) using SECRET_X and KDF_PARAMS. */
  err = derive_kek (kek_size, kdf_hash_algo, secret_x,
                    secret_x_size, kdf_params, kdf_params_size);
  if (err)
    {
      xfree (secret_x);
      return err;
    }

  /* And, finally, aeswrap with key secret_x.  */
  err = gcry_cipher_open (&hd, kdf_encr_algo, GCRY_CIPHER_MODE_AESWRAP, 0);
  if (err)
    {
      log_error ("ecdh failed to initialize AESWRAP: %s\n",
                 gpg_strerror (err));
      xfree (secret_x);
      return err;
    }

  err = gcry_cipher_setkey (hd, secret_x, kek_size);
  xfree (secret_x);
  secret_x = NULL;
  if (err)
    {
      gcry_cipher_close (hd);
      log_error ("ecdh failed in gcry_cipher_setkey: %s\n",
                 gpg_strerror (err));
    }
  else
    *r_hd = hd;

  return err;
}

/* Encrypts DATA using a key derived from the ECC shared point SHARED
   using the FIPS SP 800-56A compliant method
   key_derivation+key_wrapping.  PKEY is the public key and PK_FP the
   fingerprint of this public key.  On success the result is stored at
   R_RESULT; on failure NULL is stored at R_RESULT and an error code
   returned.  */
gpg_error_t
pk_ecdh_encrypt_with_shared_point (const char *shared, size_t nshared,
                                   const byte pk_fp[MAX_FINGERPRINT_LEN],
                                   const byte *data, size_t ndata,
                                   gcry_mpi_t *pkey, gcry_mpi_t *r_result)
{
  gpg_error_t err;
  gcry_cipher_hd_t hd;
  byte *data_buf;
  int data_buf_size;
  gcry_mpi_t result;
  byte *in;

  *r_result = NULL;

  err = prepare_ecdh_with_shared_point (shared, nshared, pk_fp, pkey, &hd);
  if (err)
    return err;

  data_buf_size = ndata;
  if ((data_buf_size & 7) != 0)
    {
      log_error ("can't use a shared secret of %d bytes for ecdh\n",
                 data_buf_size);
      gcry_cipher_close (hd);
      return gpg_error (GPG_ERR_BAD_DATA);
    }

  data_buf = xtrymalloc_secure( 1 + 2*data_buf_size + 8);
  if (!data_buf)
    {
      err = gpg_error_from_syserror ();
      gcry_cipher_close (hd);
      return err;
    }

  in = data_buf+1+data_buf_size+8;

  /* Write data MPI into the end of data_buf. data_buf is size
     aeswrap data.  */
  memcpy (in, data, ndata);

  if (DBG_CRYPTO)
    log_printhex (in, data_buf_size, "ecdh encrypting  :");

  err = gcry_cipher_encrypt (hd, data_buf+1, data_buf_size+8,
                             in, data_buf_size);
  memset (in, 0, data_buf_size);
  gcry_cipher_close (hd);
  if (err)
    {
      log_error ("ecdh failed in gcry_cipher_encrypt: %s\n",
                 gpg_strerror (err));
      xfree (data_buf);
      return err;
    }
  data_buf[0] = data_buf_size+8;

  if (DBG_CRYPTO)
    log_printhex (data_buf+1, data_buf[0], "ecdh encrypted to:");

  result = gcry_mpi_set_opaque (NULL, data_buf, 8 * (1+data_buf[0]));
  if (!result)
    {
      err = gpg_error_from_syserror ();
      xfree (data_buf);
      log_error ("ecdh failed to create an MPI: %s\n",
                 gpg_strerror (err));
      return err;
    }

  *r_result = result;
  return err;
}


static gcry_mpi_t
gen_k (unsigned nbits, int little_endian, int is_opaque)
{
  gcry_mpi_t k;

  if (is_opaque)
    {
      unsigned char *p;
      size_t nbytes = (nbits+7)/8;

      p = gcry_random_bytes_secure (nbytes, GCRY_STRONG_RANDOM);
      if ((nbits % 8))
        {
          if (little_endian)
            p[nbytes-1] &= ((1 << (nbits % 8)) - 1);
          else
            p[0] &= ((1 << (nbits % 8)) - 1);
        }
      k = gcry_mpi_set_opaque (NULL, p, nbits);
      return k;
    }

  k = gcry_mpi_snew (nbits);
  if (DBG_CRYPTO)
    log_debug ("choosing a random k of %u bits\n", nbits);

  gcry_mpi_randomize (k, nbits-1, GCRY_STRONG_RANDOM);

  if (DBG_CRYPTO)
    {
      unsigned char *buffer;
      if (gcry_mpi_aprint (GCRYMPI_FMT_HEX, &buffer, NULL, k))
        BUG ();
      log_debug ("ephemeral scalar MPI #0: %s\n", buffer);
      gcry_free (buffer);
    }

  return k;
}


/* Generate an ephemeral key for the public ECDH key in PKEY.  On
   success the generated key is stored at R_K; on failure NULL is
   stored at R_K and an error code returned.  */
gpg_error_t
pk_ecdh_generate_ephemeral_key (gcry_mpi_t *pkey, gcry_mpi_t *r_k)
{
  unsigned int nbits;
  gcry_mpi_t k;
  int is_little_endian = 0;
  int require_opaque = 0;

  if (openpgp_oid_is_cv448 (pkey[0]))
    {
      is_little_endian = 1;
      require_opaque = 1;
    }

  *r_k = NULL;

  nbits = pubkey_nbits (PUBKEY_ALGO_ECDH, pkey);
  if (!nbits)
    return gpg_error (GPG_ERR_TOO_SHORT);
  k = gen_k (nbits, is_little_endian, require_opaque);
  if (!k)
    BUG ();

  *r_k = k;
  return 0;
}



/* Perform ECDH decryption.   */
int
pk_ecdh_decrypt (gcry_mpi_t *r_result, const byte sk_fp[MAX_FINGERPRINT_LEN],
                 gcry_mpi_t data,
                 const byte *shared, size_t nshared, gcry_mpi_t * skey)
{
  gpg_error_t err;
  gcry_cipher_hd_t hd;
  size_t nbytes;
  byte *data_buf;
  int data_buf_size;
  const unsigned char *p;
  unsigned int nbits;

  *r_result = NULL;

  err = prepare_ecdh_with_shared_point (shared, nshared, sk_fp, skey, &hd);
  if (err)
    return err;

  p = gcry_mpi_get_opaque (data, &nbits);
  nbytes = (nbits+7)/8;

  data_buf_size = nbytes;
  if ((data_buf_size & 7) != 1 || data_buf_size <= 1 + 8)
    {
      log_error ("can't use a shared secret of %d bytes for ecdh\n",
                 data_buf_size);
      gcry_cipher_close (hd);
      return gpg_error (GPG_ERR_BAD_DATA);
    }

  /* The first octet is for length.  It's longer than the result
     because of one additional block of AESWRAP.   */
  data_buf_size -= 1 + 8;
  data_buf = xtrymalloc_secure (data_buf_size);
  if (!data_buf)
    {
      err = gpg_error_from_syserror ();
      gcry_cipher_close (hd);
      return err;
    }

  if (!p)
    {
      xfree (data_buf);
      gcry_cipher_close (hd);
      return gpg_error (GPG_ERR_BAD_MPI);
    }
  if (p[0] != nbytes-1)
    {
      log_error ("ecdh inconsistent size\n");
      xfree (data_buf);
      gcry_cipher_close (hd);
      return gpg_error (GPG_ERR_BAD_MPI);
    }

  if (DBG_CRYPTO)
    log_printhex (p+1, nbytes-1, "ecdh decrypting :");

  err = gcry_cipher_decrypt (hd, data_buf, data_buf_size, p+1, nbytes-1);
  gcry_cipher_close (hd);
  if (err)
    {
      log_error ("ecdh failed in gcry_cipher_decrypt: %s\n",
                 gpg_strerror (err));
      xfree (data_buf);
      return err;
    }

  if (DBG_CRYPTO)
    log_printhex (data_buf, data_buf_size, "ecdh decrypted to :");

  /* Padding is removed later.  */
  /* if (in[data_buf_size-1] > 8 ) */
  /*   { */
  /*     log_error ("ecdh failed at decryption: invalid padding." */
  /*                " 0x%02x > 8\n", in[data_buf_size-1] ); */
  /*     return gpg_error (GPG_ERR_BAD_KEY); */
  /*   } */

  err = gcry_mpi_scan (r_result, GCRYMPI_FMT_USG, data_buf,
                       data_buf_size, NULL);
  xfree (data_buf);
  if (err)
    {
      log_error ("ecdh failed to create a plain text MPI: %s\n",
                 gpg_strerror (err));
      return err;
    }

  return err;
}
