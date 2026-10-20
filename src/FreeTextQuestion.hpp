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

#if !defined(INCLUDED_FREETEXTQUESTION_H)
#define INCLUDED_FREETEXTQUESTION_H

#include "Question.hpp"

/**
 * Question class that allows free-format text input.
 **/
class FreeTextQuestion : public Question {
public:
  /**
   * Define a list of options to show following the question,
   * to show the user what values are acceptable.
   **/
  void setOptions(std::string options) { mOptions = options; }

  /**
   * An empty answer (and no default) is not an answer.
   **/
  void setRequired(bool required) { mRequired = required; }

  /**
   * Ask the question, and return the answer.
   **/
  virtual std::string ask() {
    for (;;) {
      std::string prompt = mQuestion;

      /* If there is an options list, display it after the
       * question enclosed in ().
       */
      if (mOptions != "")
        prompt += " (" + mOptions + ")";

      /* If there is a default value, display it after the
       * question enclosed in [].
       */
      if (mDefault != "")
        prompt += " [" + mDefault + "]";

      mAnswer = readAnswer(prompt + ": ");

      /* If the question is answered with '?', display the
       * explanation, then ask again.
       */
      if (mAnswer == "?" && !mScripted) {
        explain();
        continue;
      }

      /* If the question is answered with <return>, set the
       * answer to the default answer.
       */
      if (mAnswer == "")
        mAnswer = mDefault;

      if (mRequired && mAnswer == "") {
        if (mScripted)
          refuseScripted(mAnswer, "an answer is needed");
        std::cout << "\nAn answer is needed here, or '?' for help.\n\n";
        continue;
      }

      /* Return the answer.
       */
      return mAnswer;
    }
  }

protected:
  /** List of options to show after the question. */
  std::string mOptions;

  /** Whether an empty answer is refused. */
  bool mRequired = false;
};

#endif // !defined(INCLUDED_FREETEXTQUESTION_H)
