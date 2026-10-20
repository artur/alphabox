/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

#if !defined(INCLUDED_NUMBERQUESTION_H)
#define INCLUDED_NUMBERQUESTION_H

#include "FreeTextQuestion.hpp"

#include <sstream>

/**
 * Convert an integer to a string.
 **/
inline std::string i2s(int x) { return std::to_string(x); }

/**
 * Convert a string to an integer: decimal digits only (a leading '-' for a
 * negative number). Throws CLogicException for anything else.
 **/
inline int s2i(const std::string x) {
  size_t i = (!x.empty() && x[0] == '-') ? 1 : 0;
  if (i == x.size() || x.size() - i > 9)
    throw CLogicException("invalid conversion");
  for (size_t j = i; j < x.size(); j++)
    if (x[j] < '0' || x[j] > '9')
      throw CLogicException("invalid conversion");
  return std::stoi(x);
}

/**
 * Question class implementing a question that has an integer number as
 * the answer.
 **/
class NumberQuestion : public FreeTextQuestion {
public:
  /**
   * Define the allowable range for the answer.
   **/
  void setRange(int low, int high) {
    mLow = low;
    mHigh = high;
  }

  /**
   * Convert the answer to a number.
   **/
  int getNum() { return s2i(mAnswer); }

  /**
   * Ask the question and return the answer.
   **/
  virtual std::string ask() {
    /* Set the options list to (low-high).
     */
    setOptions(i2s(mLow) + "-" + i2s(mHigh));

    /* Keep repeating the question until a valid
     * answer is received.
     */
    for (;;) {
      /* Ask the question as a FreeTextQuestion. This
       * takes care of the explanation and default
       * value handling.
       */
      FreeTextQuestion::ask();

      try {
        /* Convert the answer to an integer.
         */
        int value = s2i(mAnswer);

        /* If the answer is within the allowed range,
         * return it.
         */
        if (value >= mLow && value <= mHigh)
          return mAnswer;

        /* The answer is out of range.
         */
        if (mScripted)
          refuseScripted(mAnswer, "not in " + i2s(mLow) + "-" + i2s(mHigh));
        std::cout << "\nPlease enter a value that is within the indicated "
                     "range, or '?' for help.\n\n";
      } catch (CLogicException &) {
        /* The answer is not a number.
         */
        if (mScripted)
          refuseScripted(mAnswer, "not a number");
        std::cout << "\nPlease enter an integer value.\n\n";
      }
    }
  }

protected:
  /** Low limit of the allowed range. */
  int mLow = 0;
  /** High limit of the allowed range. */
  int mHigh = 0;
};

#endif // !defined(INCLUDED_NUMBERQUESTION_H)
