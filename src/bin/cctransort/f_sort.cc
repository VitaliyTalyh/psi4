/*
 * PSI4: an ab initio quantum chemistry software package
 *
 * Copyright (c) 2007-2015 The PSI4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <libdpd/dpd.h>

namespace psi { namespace cctransort {

void f_sort(int reference)
{
  dpdbuf4 F;

  if(reference == 2) {  /*** UHF ***/
    global_dpd_->buf4_init(&F, PSIF_CC_FINTS, 0, 28, 26, 28, 26, 0, "F <Ab|Ci>");
    global_dpd_->buf4_sort_ooc(&F, PSIF_CC_FINTS, spqr, 27, 29, "F <iA|bC>");
    global_dpd_->buf4_close(&F);
  }
}

}} // namespace psi::cctransort
