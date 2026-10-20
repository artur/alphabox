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

#if !defined(INCLUDED_QUESTION_H)
#define INCLUDED_QUESTION_H

#include "Exception.hpp"

#include <deque>
#include <iostream>
#include <map>
#include <string>

/**
 * Abstract Question base class.
 *
 * A question has a key (`platform`, `nic.type`), by which an answer can be
 * given before the session: `alphabox configure --set key=value` or a line
 * of an --answers file. Each time a question is asked it takes the next
 * answer queued under its key; with --defaults, a question that has none
 * takes its default; otherwise it is asked on stdin. An answer from the
 * command line that a question does not accept ends the session, as does
 * input that ends (CAbortException).
 **/
class Question {
public:
  virtual ~Question() {}

  /**
   * Return the answer previously given.
   **/
  std::string getAnswer() { return mAnswer; }

  /**
   * Set the answer.
   **/
  void setAnswer(std::string answer) { mAnswer = answer; }

  /**
   * Define an explanation for the question, that will be shown when
   * the question is answered with '?'.
   **/
  void setExplanation(std::string explanation) { mExplanation = explanation; }

  /**
   * Define the question to ask.
   **/
  void setQuestion(std::string question) { mQuestion = question; }

  /**
   * Define a default value to use when the question is answered
   * with a <return>.
   **/
  void setDefault(std::string defval) { mDefault = defval; }

  /**
   * The key by which the answer can be given beforehand.
   **/
  void setKey(std::string key) { mKey = key; }

  /**
   * Ask the question, and return the value given.
   **/
  virtual std::string ask() = 0;

  /**
   * Display the explanation.
   **/
  virtual void explain() {
    std::cout << "\nEXPLANATION:\n" << mExplanation << "\n\n";
  }

  /// An answer given beforehand for the question with this key.
  static void queueAnswer(const std::string &key, const std::string &value) {
    queued()[key].push_back(value);
  }
  /// Questions without an answer given beforehand take their default.
  static void useDefaults(bool on) { defaults() = on; }
  /// The keys with answers given beforehand that no question took.
  static std::string unusedAnswers() {
    std::string s;
    for (const auto &k : queued())
      if (!k.second.empty())
        s += (s.empty() ? "" : ", ") + k.first + "=" + k.second.front();
    return s;
  }

protected:
  /**
   * Show the question, after its key in braces, and get the raw answer:
   * the next one queued under the key, the default (--defaults), or a line
   * from stdin. A scripted answer is shown after the question, so the
   * output reads as the session would have.
   **/
  std::string readAnswer(const std::string &prompt) {
    if (!mKey.empty())
      std::cout << "{" << mKey << "} ";
    std::cout << prompt;
    auto q = queued().find(mKey);
    if (!mKey.empty() && q != queued().end() && !q->second.empty()) {
      mScripted = true;
      std::string a = q->second.front();
      q->second.pop_front();
      std::cout << a << "\n";
      return a;
    }
    if (defaults()) {
      mScripted = true;
      std::cout << mDefault << "\n";
      return mDefault;
    }
    mScripted = false;
    std::string a;
    if (!std::getline(std::cin, a))
      throw CAbortException("the input ended before the configuration was "
                            "complete");
    return a;
  }

  /// A scripted answer the question cannot take ends the session.
  void refuseScripted(const std::string &answer, const std::string &why) {
    throw CAbortException((mKey.empty() ? std::string("answer") : mKey) +
                          " = \"" + answer + "\": " + why);
  }

  /** The stored answer. */
  std::string mAnswer;

  /** An explanation for the question. */
  std::string mExplanation;

  /** The question to ask. */
  std::string mQuestion;

  /** Default value. */
  std::string mDefault;

  /** The key for answers given beforehand. */
  std::string mKey;

  /** Whether the last answer came from the command line or --defaults. */
  bool mScripted = false;

private:
  static std::map<std::string, std::deque<std::string>> &queued() {
    static std::map<std::string, std::deque<std::string>> q;
    return q;
  }
  static bool &defaults() {
    static bool d = false;
    return d;
  }
};

#endif // !defined(INCLUDED_QUESTION_H)
