/**
 * @Copyright (c) 2016-2018 - Dominique Lavenier & UPMEM
 */

#ifndef __INTEGRATION_NODP_H__
#define __INTEGRATION_NODP_H__

/**
 * @brief Compares two sequences of symbols to assign a score, based on the number of substitutions and INDELs
 *
 * The function computes the distance between two sequences, if there are only substitutions, or the detection
 * of INDELs.
 *
 * @param s1           The first sequence, packed in a byte stream
 * @param s2           The second sequence, packed in a byte stream
 * @param nbr_len      The size of each vector, in bytes
 * @param delta        Delta to apply to the comparison depending on the round
 * @param max_score    A threshold, beyond which there is no need to go further (because we already have a better score)
 *
 * @return -1 if INDELs are detected, otherwise a score specifying the distance between s1 and s2 with substitutions
 */
int noDP(uint8_t *s1, uint8_t *s2, unsigned int nbr_len, unsigned int delta, int max_score);

#endif //INTEGRATION_NODP_H
