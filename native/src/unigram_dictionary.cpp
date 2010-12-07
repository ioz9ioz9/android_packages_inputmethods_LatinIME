/*
**
** Copyright 2010, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "LatinIME: unigram_dictionary.cpp"

#include "basechars.h"
#include "char_utils.h"
#include "dictionary.h"
#include "unigram_dictionary.h"

namespace latinime {

UnigramDictionary::UnigramDictionary(const unsigned char *dict, int typedLetterMultiplier,
        int fullWordMultiplier, int maxWordLength, int maxWords, int maxAlternatives,
        const bool isLatestDictVersion)
    : DICT(dict), MAX_WORD_LENGTH(maxWordLength),MAX_WORDS(maxWords),
    MAX_ALTERNATIVES(maxAlternatives), IS_LATEST_DICT_VERSION(isLatestDictVersion),
    TYPED_LETTER_MULTIPLIER(typedLetterMultiplier), FULL_WORD_MULTIPLIER(fullWordMultiplier) {
    LOGI("UnigramDictionary - constructor");
}

UnigramDictionary::~UnigramDictionary() {}

int UnigramDictionary::getSuggestions(int *codes, int codesSize, unsigned short *outWords,
        int *frequencies, int *nextLetters, int nextLettersSize)
{

    initSuggestions(codes, codesSize, outWords, frequencies);

    int suggestedWordsCount = getSuggestionCandidates(codesSize, -1, nextLetters,
            nextLettersSize);

    // If there aren't sufficient suggestions, search for words by allowing wild cards at
    // the different character positions. This feature is not ready for prime-time as we need
    // to figure out the best ranking for such words compared to proximity corrections and
    // completions.
    if (SUGGEST_MISSING_CHARACTERS && suggestedWordsCount < SUGGEST_MISSING_CHARACTERS_THRESHOLD) {
        for (int i = 0; i < codesSize; ++i) {
            int tempCount = getSuggestionCandidates(codesSize, i, NULL, 0);
            if (tempCount > suggestedWordsCount) {
                suggestedWordsCount = tempCount;
                break;
            }
        }
    }

    if (DEBUG_DICT) {
        LOGI("Returning %d words", suggestedWordsCount);
        LOGI("Next letters: ");
        for (int k = 0; k < nextLettersSize; k++) {
            if (nextLetters[k] > 0) {
                LOGI("%c = %d,", k, nextLetters[k]);
            }
        }
        LOGI("\n");
    }
    return suggestedWordsCount;
}

void UnigramDictionary::initSuggestions(int *codes, int codesSize, unsigned short *outWords,
        int *frequencies) {
    if (DEBUG_DICT) LOGI("initSuggest");
    mFrequencies = frequencies;
    mOutputChars = outWords;
    mInputCodes = codes;
    mInputLength = codesSize;
    mMaxEditDistance = mInputLength < 5 ? 2 : mInputLength / 2;
}

int UnigramDictionary::getSuggestionCandidates(int inputLength, int skipPos,
        int *nextLetters, int nextLettersSize) {
    if (DEBUG_DICT) LOGI("getSuggestionCandidates");
    int initialPos = 0;
    if (IS_LATEST_DICT_VERSION) {
        initialPos = DICTIONARY_HEADER_SIZE;
    }
    getWords(initialPos, inputLength, skipPos, nextLetters, nextLettersSize);

    // Get the word count
    int suggestedWordsCount = 0;
    while (suggestedWordsCount < MAX_WORDS && mFrequencies[suggestedWordsCount] > 0) {
        suggestedWordsCount++;
    }
    return suggestedWordsCount;
}

void UnigramDictionary::registerNextLetter(
        unsigned short c, int *nextLetters, int nextLettersSize) {
    if (c < nextLettersSize) {
        nextLetters[c]++;
    }
}

bool UnigramDictionary::addWord(unsigned short *word, int length, int frequency) {
    word[length] = 0;
    if (DEBUG_DICT) {
        char s[length + 1];
        for (int i = 0; i <= length; i++) s[i] = word[i];
        LOGI("Found word = %s, freq = %d : \n", s, frequency);
    }
    if (length > MAX_WORD_LENGTH) {
        if (DEBUG_DICT) LOGI("Exceeded max word length.");
        return false;
    }

    // Find the right insertion point
    int insertAt = 0;
    while (insertAt < MAX_WORDS) {
        if (frequency > mFrequencies[insertAt] || (mFrequencies[insertAt] == frequency
                && length < Dictionary::wideStrLen(mOutputChars + insertAt * MAX_WORD_LENGTH))) {
            break;
        }
        insertAt++;
    }
    if (insertAt < MAX_WORDS) {
        memmove((char*) mFrequencies + (insertAt + 1) * sizeof(mFrequencies[0]),
               (char*) mFrequencies + insertAt * sizeof(mFrequencies[0]),
               (MAX_WORDS - insertAt - 1) * sizeof(mFrequencies[0]));
        mFrequencies[insertAt] = frequency;
        memmove((char*) mOutputChars + (insertAt + 1) * MAX_WORD_LENGTH * sizeof(short),
               (char*) mOutputChars + insertAt * MAX_WORD_LENGTH * sizeof(short),
               (MAX_WORDS - insertAt - 1) * sizeof(short) * MAX_WORD_LENGTH);
        unsigned short *dest = mOutputChars + insertAt * MAX_WORD_LENGTH;
        while (length--) {
            *dest++ = *word++;
        }
        *dest = 0; // NULL terminate
        if (DEBUG_DICT) LOGI("Added word at %d\n", insertAt);
        return true;
    }
    return false;
}

unsigned short UnigramDictionary::toLowerCase(unsigned short c) {
    if (c < sizeof(BASE_CHARS) / sizeof(BASE_CHARS[0])) {
        c = BASE_CHARS[c];
    }
    if (c >='A' && c <= 'Z') {
        c |= 32;
    } else if (c > 127) {
        c = latin_tolower(c);
    }
    return c;
}

bool UnigramDictionary::sameAsTyped(unsigned short *word, int length) {
    if (length != mInputLength) {
        return false;
    }
    int *inputCodes = mInputCodes;
    while (length--) {
        if ((unsigned int) *inputCodes != (unsigned int) *word) {
            return false;
        }
        inputCodes += MAX_ALTERNATIVES;
        word++;
    }
    return true;
}

static const char QUOTE = '\'';

void UnigramDictionary::getWords(const int initialPos, const int inputLength, const int skipPos,
        int *nextLetters, const int nextLettersSize) {
    int initialPosition = initialPos;
    const int count = Dictionary::getCount(DICT, &initialPosition);
    getWordsRec(count, initialPosition, 0,
            min(inputLength * MAX_DEPTH_MULTIPLIER, MAX_WORD_LENGTH),
            mInputLength <= 0, 1, 0, 0, skipPos, nextLetters, nextLettersSize);
}

// snr : frequency?
void UnigramDictionary::getWordsRec(const int childrenCount, const int pos, const int depth,
        const int maxDepth, const bool traverseAllNodes, const int snr, const int inputIndex,
        const int diffs, const int skipPos, int *nextLetters, const int nextLettersSize) {
    int siblingPos = pos;
    for (int i = 0; i < childrenCount; ++i) {
        int newCount;
        int newChildPosition;
        int newDepth;
        bool newTraverseAllNodes;
        int newSnr;
        int newInputIndex;
        int newDiffs;
        int newSiblingPos;
        const bool needsToTraverseChildrenNodes = processCurrentNode(siblingPos, depth, maxDepth,
                traverseAllNodes, snr, inputIndex, diffs, skipPos, nextLetters, nextLettersSize,
                &newCount, &newChildPosition, &newDepth, &newTraverseAllNodes, &newSnr,
                &newInputIndex, &newDiffs, &newSiblingPos);
        siblingPos = newSiblingPos;

        if (needsToTraverseChildrenNodes) {
            getWordsRec(newCount, newChildPosition, newDepth, maxDepth, newTraverseAllNodes,
                    newSnr, newInputIndex, newDiffs, skipPos, nextLetters, nextLettersSize);
        }
    }
}

inline void UnigramDictionary::onTerminalWhenUserTypedLengthIsGreaterThanInputLength(
        unsigned short *word, const int inputLength, const int depth, const int snr,
        int *nextLetters, const int nextLettersSize, const int skipPos, const int freq) {
    addWord(word, depth + 1, freq * snr);
    if (depth >= inputLength && skipPos < 0) {
        registerNextLetter(mWord[mInputLength], nextLetters, nextLettersSize);
    }
}

inline void UnigramDictionary::onTerminalWhenUserTypedLengthIsSameAsInputLength(
        unsigned short *word, const int depth, const int snr, const int skipPos, const int freq,
        const int addedWeight) {
    if (!sameAsTyped(word, depth + 1)) {
        int finalFreq = freq * snr * addedWeight;
        // Proximity collection will promote a word of the same length as
        // what user typed.
        if (skipPos < 0) finalFreq *= FULL_WORD_MULTIPLIER;
        addWord(word, depth + 1, finalFreq);
    }
}

inline bool UnigramDictionary::needsToSkipCurrentNode(const unsigned short c,
        const int inputIndex, const int skipPos, const int depth) {
    const unsigned short userTypedChar = (mInputCodes + (inputIndex * MAX_ALTERNATIVES))[0];
    // Skip the ' or other letter and continue deeper
    return (c == QUOTE && userTypedChar != QUOTE) || skipPos == depth;
}

inline int UnigramDictionary::getMatchedProximityId(const int *currentChars,
        const unsigned short c, const int skipPos) {
    const unsigned short lowerC = toLowerCase(c);
    int j = 0;
    while (currentChars[j] > 0) {
        const bool matched = (currentChars[j] == lowerC || currentChars[j] == c);
        // If skipPos is defined, not to search proximity collections.
        // First char is what user typed.
        if (matched) {
            return j;
        } else if (skipPos >= 0) {
            return -1;
        }
        ++j;
    }
    return -1;
}

inline bool UnigramDictionary::processCurrentNode(const int pos, const int depth,
        const int maxDepth, const bool traverseAllNodes, const int snr, const int inputIndex,
        const int diffs, const int skipPos, int *nextLetters, const int nextLettersSize,
        int *newCount, int *newChildPosition, int *newDepth, bool *newTraverseAllNodes,
        int *newSnr, int*newInputIndex, int *newDiffs, int *nextSiblingPosition) {
    unsigned short c;
    int childPosition;
    bool terminal;
    int freq;
    *nextSiblingPosition = Dictionary::setDictionaryValues(DICT, IS_LATEST_DICT_VERSION, pos, &c,
            &childPosition, &terminal, &freq);

    const bool needsToTraverseChildrenNodes = childPosition != 0;

    // If we are only doing traverseAllNodes, no need to look at the typed characters.
    if (traverseAllNodes || needsToSkipCurrentNode(c, inputIndex, skipPos, depth)) {
        mWord[depth] = c;
        if (traverseAllNodes && terminal) {
            onTerminalWhenUserTypedLengthIsGreaterThanInputLength(mWord, mInputLength, depth,
                    snr, nextLetters, nextLettersSize, skipPos, freq);
        }
        if (!needsToTraverseChildrenNodes) return false;
        *newTraverseAllNodes = traverseAllNodes;
        *newSnr = snr;
        *newDiffs = diffs;
        *newInputIndex = inputIndex;
        *newDepth = depth + 1;
    } else {
        int *currentChars = mInputCodes + (inputIndex * MAX_ALTERNATIVES);
        int matchedProximityCharId = getMatchedProximityId(currentChars, c, skipPos);
        if (matchedProximityCharId < 0) return false;
        mWord[depth] = c;
        // If inputIndex is greater than mInputLength, that means there is no
        // proximity chars. So, we don't need to check proximity.
        const int addedWeight = matchedProximityCharId == 0 ? TYPED_LETTER_MULTIPLIER : 1;
        const bool isSameAsUserTypedLength = mInputLength == inputIndex + 1;
        if (isSameAsUserTypedLength && terminal) {
            onTerminalWhenUserTypedLengthIsSameAsInputLength(mWord, depth, snr,
                    skipPos, freq, addedWeight);
        }
        if (!needsToTraverseChildrenNodes) return false;
        // Start traversing all nodes after the index exceeds the user typed length
        *newTraverseAllNodes = isSameAsUserTypedLength;
        *newSnr = snr * addedWeight;
        *newDiffs = diffs + (matchedProximityCharId > 0);
        *newInputIndex = inputIndex + 1;
        *newDepth = depth + 1;
    }
    // Optimization: Prune out words that are too long compared to how much was typed.
    if (*newDepth > maxDepth || *newDiffs > mMaxEditDistance) {
        return false;
    }

    // If inputIndex is greater than mInputLength, that means there are no proximity chars.
    if (mInputLength <= *newInputIndex) {
        *newTraverseAllNodes = true;
    }
    // get the count of nodes and increment childAddress.
    *newCount = Dictionary::getCount(DICT, &childPosition);
    *newChildPosition = childPosition;
    if (DEBUG_DICT) assert(needsToTraverseChildrenNodes);
    return needsToTraverseChildrenNodes;
}

} // namespace latinime
