/**
 * @file rules_en.h
 * @brief English grapheme-to-phoneme rules for OOV words
 * 
 * Fallback rules for words not in dictionary.
 * Based on common English orthography patterns.
 */

#ifndef ETHERVOX_RULES_EN_H
#define ETHERVOX_RULES_EN_H

#include <stddef.h>

/**
 * Apply English G2P rules to unknown word
 * @param word Input word (lowercase)
 * @param arpabet_out Output ARPAbet (space-separated)
 * @param max_len Maximum output length
 * @return 0 on success, -1 on error
 */
int apply_english_g2p_rules(const char* word, char* arpabet_out, size_t max_len);

#endif // ETHERVOX_RULES_EN_H
