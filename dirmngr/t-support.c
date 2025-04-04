/* t-support.c - Module test support (stubs etc).
 * Copyright (C) 2018 Hasanur Rahevy
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
 * SPDX-License-Identifier: GPL-3.0+
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>


#include "../common/util.h"
#include "dirmngr-status.h"
#include "t-support.h"



/* Stub for testing. See server.c for the real implementation.  */
gpg_error_t
dirmngr_status_printf (ctrl_t ctrl, const char *keyword,
                       const char *format, ...)
{
  (void)ctrl;
  (void)keyword;
  (void)format;

  return 0;
}
