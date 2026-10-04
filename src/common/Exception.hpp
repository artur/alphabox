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

#if !defined(INCLUDED_EXCEPTION_H)
#define INCLUDED_EXCEPTION_H

#include <exception>
#include <string>

/**
 * \brief What FAILURE() throws (es40_debug.hpp): CException and the classes
 * it names.
 *
 * FAILURE(cls, msg) throws C<cls>Exception(msg, where), where saying the
 * file, line and function. The classes, their names and their hierarchy are
 * those of the Poco-derived exceptions that lived in src/base, cut down to
 * the ones FAILURE names and the bases they derive from; what() is the
 * class's name and displayText() "name: message: where", as before.
 **/
class CException : public std::exception {
public:
  explicit CException(const std::string &msg = std::string()) : m_msg(msg) {}
  CException(const std::string &msg, const std::string &arg) : m_msg(msg) {
    if (!arg.empty()) {
      m_msg.append(": ");
      m_msg.append(arg);
    }
  }

  /// The exception's class, in words ("Runtime exception").
  virtual const char *name() const noexcept { return "Exception"; }
  const char *what() const noexcept override { return name(); }
  const std::string &message() const { return m_msg; }

  /// "name: message", or the name alone when there is no message.
  std::string displayText() const {
    std::string txt = name();
    if (!m_msg.empty()) {
      txt.append(": ");
      txt.append(m_msg);
    }
    return txt;
  }

private:
  std::string m_msg;
};

#define ALPHABOX_EXCEPTION(CLS, BASE, NAME)                                    \
  class CLS : public BASE {                                                    \
  public:                                                                      \
    using BASE::BASE;                                                          \
    const char *name() const noexcept override { return NAME; }                \
  };

ALPHABOX_EXCEPTION(CLogicException, CException, "Logic exception")
ALPHABOX_EXCEPTION(CInvalidArgumentException, CLogicException,
                   "Invalid argument")
ALPHABOX_EXCEPTION(CNotImplementedException, CLogicException, "Not implemented")
ALPHABOX_EXCEPTION(CIllegalStateException, CLogicException, "Illegal state")

ALPHABOX_EXCEPTION(CRuntimeException, CException, "Runtime exception")
ALPHABOX_EXCEPTION(CTimeoutException, CRuntimeException, "Timeout")
ALPHABOX_EXCEPTION(COutOfMemoryException, CRuntimeException, "Out of memory")
ALPHABOX_EXCEPTION(CIOException, CRuntimeException, "I/O error")
ALPHABOX_EXCEPTION(CFileException, CIOException, "File access error")
ALPHABOX_EXCEPTION(CFileNotFoundException, CFileException, "File not found")

ALPHABOX_EXCEPTION(CConfigurationException, CException, "Configuration error")
ALPHABOX_EXCEPTION(CThreadException, CException, "Threading error")
ALPHABOX_EXCEPTION(CWin32Exception, CException, "Win32 error")
ALPHABOX_EXCEPTION(CSDLException, CException, "SDL error")
/// The user asked to exit.
ALPHABOX_EXCEPTION(CGracefulException, CException, "Graceful exit")
/// The user asked to abort.
ALPHABOX_EXCEPTION(CAbortException, CException, "Abort requested")

#undef ALPHABOX_EXCEPTION

#endif // !defined(INCLUDED_EXCEPTION_H)
