/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#if !defined(INCLUDED_DYNLIB_H)
#define INCLUDED_DYNLIB_H

#if !defined(_WIN32)
/**
 * \brief Open a shared library at first use (libslirp, libpcap), so the
 * binary starts where the library is not installed. Windows loads wpcap.dll
 * its own way (NetworkPcap.cpp).
 *
 * \param names         file names to try, null terminated, best first; each
 *                      is first given to dlopen as is (the system search)
 * \param dirs          directories also searched for them, null terminated
 *                      (may be null)
 * \param fallback_dir  the directory pkg-config reported at build time
 *                      (may be null or empty), searched last
 * \return the handle, or null.
 */
void *dynlib_open(const char *const *names, const char *const *dirs,
                  const char *fallback_dir);

/// A symbol of an open library, or null.
void *dynlib_sym(void *handle, const char *name);
#endif

#endif // !defined(INCLUDED_DYNLIB_H)
