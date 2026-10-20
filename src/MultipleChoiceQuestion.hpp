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

#if !defined(INCLUDED_MULTIPLECHOICEQUESTION_H)
#define INCLUDED_MULTIPLECHOICEQUESTION_H

#include "FreeTextQuestion.hpp"

#include <algorithm>
#include <vector>

/**
 * Class defining a possible answer to a multiple
 * choice question.
 **/
class Answer {
public:
  /**
   * Constructor.
   *
   * Defines the answer, value and explanation of
   * this answer.
   **/
  Answer(std::string answer, std::string value, std::string explanation)
      : mAnswer(answer), mValue(value), mExplanation(explanation) {}

  /**
   * Equality operator. Matches on Answer.
   **/
  bool operator==(const std::string &x) const { return (mAnswer == x); }
  /**
   * Get the answer.
   **/
  std::string getAnswer() const { return mAnswer; }

  /**
   * Get the value.
   **/
  std::string getValue() const { return mValue; }

  /**
   * Get the explanation.
   **/
  std::string getExplanation() const { return mExplanation; }

protected:
  /** Answer as it should be entered by the user. */
  std::string mAnswer;

  /** Value as it is output to the config file. */
  std::string mValue;

  /** Explanation shown when '?' is given as an answer. */
  std::string mExplanation;
};

/**
 * Vector of possible answers to a multiple choice question.
 **/
typedef std::vector<Answer> AnswerSet;

/**
 * Question class implementing a multiple choice question.
 *
 * The question is asked as a FreeTextQuestion, and the answer is checked
 * against the answers given. ask() returns the answer's value, which is
 * what getAnswer() gives afterwards too.
 **/
class MultipleChoiceQuestion : public FreeTextQuestion {
public:
  /**
   * Add an answer to the set of allowed answers.
   **/
  void addAnswer(std::string answer, std::string value,
                 std::string explanation) {
    mAnswerSet.push_back(Answer(answer, value, explanation));
  }

  /**
   * Explain the question.
   **/
  virtual void explain() {
    /* Explain the question as usual.
     */
    FreeTextQuestion::explain();

    /* Explain the allowed answers.
     */
    std::cout << "POSSIBLE VALUES:\n";
    for (const Answer &a : mAnswerSet)
      std::cout << a.getAnswer() << "\t\t" << a.getExplanation() << "\n";
    std::cout << "\n";
  }

  /**
   * Get the number of answers in the answer set.
   **/
  size_t countAnswers() { return mAnswerSet.size(); }

  /**
   * Ask the question.
   **/
  virtual std::string ask() {
    /* The options list; a long one is cut short ('?' lists them all).
     */
    std::string options;
    for (size_t i = 0; i < mAnswerSet.size(); i++) {
      if (i == 12 && mAnswerSet.size() > 16) {
        options += ",... (" + std::to_string(mAnswerSet.size()) + ")";
        break;
      }
      options += (i ? "," : "") + mAnswerSet[i].getAnswer();
    }
    setOptions(options);

    /* Keep repeating the question until a valid
     * answer is received.
     */
    for (;;) {
      /* Ask the question as a FreeTextQuestion. This
       * takes care of the explanation and default
       * value handling.
       */
      FreeTextQuestion::ask();

      /* Try to find the answer received in the answer
       * set.
       */
      AnswerSet::iterator itAnswers =
          std::find(mAnswerSet.begin(), mAnswerSet.end(), mAnswer);

      /* Has a matching answer be found?
       */
      if (itAnswers != mAnswerSet.end()) {
        /* Get the value for the received answer.
         */
        std::string value = itAnswers->getValue();
        haveChosen(mAnswer);
        mAnswer = value;

        /* Return the value.
         */
        return mAnswer;
      }

      /* No valid, matching answer received.
       */
      if (mScripted)
        refuseScripted(mAnswer, "not one of " + allAnswers());
      std::cout << "\nPlease enter a valid value, or '?' for help.\n\n";
    }
  };

  /**
   * Remove one answer from the answer set.
   **/
  void dropChoice(std::string choice) {
    AnswerSet::iterator itAnswers =
        std::find(mAnswerSet.begin(), mAnswerSet.end(), choice);
    if (itAnswers != mAnswerSet.end())
      mAnswerSet.erase(itAnswers);
  }

  /**
   * Override this to do something special with
   * the answer received.
   **/
  virtual void haveChosen(std::string choice) { (void)choice; }

  /**
   * Return the first possible answer, or "" when there is none.
   **/
  std::string getFirstChoice() {
    return mAnswerSet.empty() ? std::string() : mAnswerSet[0].getAnswer();
  }

  /**
   * Whether `answer` is one of the answers.
   **/
  bool hasChoice(const std::string &answer) {
    return std::find(mAnswerSet.begin(), mAnswerSet.end(), answer) !=
           mAnswerSet.end();
  }

protected:
  std::string allAnswers() {
    std::string s;
    for (const Answer &a : mAnswerSet)
      s += (s.empty() ? "" : ", ") + a.getAnswer();
    return s;
  }

  /** Set containing all allowed answers. */
  AnswerSet mAnswerSet;
};

#endif // !defined(INCLUDED_MULTIPLECHOICEQUESTION_H)
