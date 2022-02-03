/**
 * Copyright 2016-2019 - Dominique Lavenier & UPMEM
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>

#include "accumulateread.h"
#include "dispatch.h"
#include "index.h"
#include "mram_dpu.h"
#include "parse_args.h"
#include "simu_backend.h"
#include "upvc.h"

#include <dpu.h>

#include "common.h"

#define MAX_SCORE 40

#define FOREACH_THREAD(it) for (unsigned int it = 0; it < get_nb_thread_for_simu(); it++)

static coords_and_nbr_t **mrams;
static const int delta_neighbour = 0;

static pthread_barrier_t barrier;
static pthread_t *tids;
static bool stop_threads = false;
static unsigned int dpu_offset_shared;
static unsigned int pass_id_shared;

static int min(int a, int b) { return a < b ? a : b; }

#define PQD_INIT_VAL (999)
static void ODPD_compute(
    int i, int j, int8_t *s1, int8_t *s2, int *Pppj, int Pppjm, int *Qppj, int Qlpj, int *Dppj, int Dppjm, int Dlpj, int Dlpjm)
{
    int d = Dlpjm;
    int QP;
    *Pppj = min(Dppjm + COST_GAPO, Pppjm + COST_GAPE);
    *Qppj = min(Dlpj + COST_GAPO, Qlpj + COST_GAPE);
    QP = min(*Pppj, *Qppj);
    if (((s1[(i - 1) / 4] >> (2 * ((i - 1) % 4))) & 3) != ((s2[(j - 1) / 4] >> (2 * ((j - 1) % 4))) & 3)) {
        d += COST_SUB;
    }
    *Dppj = min(d, QP);
}

/**
 * @brief Compute the alignment distance by dynamical programming on the diagonals of the matrix.
 * Stops when score is greater than max_score
 */
static int ODPD(int8_t *s1, int8_t *s2, int max_score, int size_neighbour_in_symbols)
{
    int matrix_size = size_neighbour_in_symbols + 1;
    int D[2][matrix_size];
    int P[2][matrix_size];
    int Q[2][matrix_size];
    int diagonal = (NB_DIAG / 2) + 1;

    for (int j = 0; j <= diagonal; j++) {
        P[0][j] = PQD_INIT_VAL;
        Q[0][j] = PQD_INIT_VAL;
        D[0][j] = j * COST_SUB;
    }
    P[1][0] = PQD_INIT_VAL;
    Q[1][0] = PQD_INIT_VAL;

    for (int i = 1; i < diagonal; i++) {
        int min_score = PQD_INIT_VAL;
        int pp = i % 2;
        int lp = (i - 1) % 2;
        D[pp][0] = i * COST_SUB;
        for (int j = 1; j < i + diagonal; j++) {
            ODPD_compute(
                i, j, s1, s2, &P[pp][j], P[pp][j - 1], &Q[pp][j], Q[lp][j], &D[pp][j], D[pp][j - 1], D[lp][j], D[lp][j - 1]);
            if (D[pp][j] < min_score) {
                min_score = D[pp][j];
            }
        }
        Q[pp][i + diagonal] = PQD_INIT_VAL;
        D[pp][i + diagonal] = PQD_INIT_VAL;
        if (min_score > max_score) {
            return min_score;
        }
    }

    for (int i = diagonal; i < matrix_size - diagonal; i++) {
        int min_score = PQD_INIT_VAL;
        int pp = i % 2;
        int lp = (i - 1) % 2;
        P[pp][i - diagonal] = PQD_INIT_VAL;
        D[pp][i - diagonal] = PQD_INIT_VAL;
        for (int j = i + 1 - diagonal; j < i + diagonal; j++) {
            ODPD_compute(
                i, j, s1, s2, &P[pp][j], P[pp][j - 1], &Q[pp][j], Q[lp][j], &D[pp][j], D[pp][j - 1], D[lp][j], D[lp][j - 1]);
            if (D[pp][j] < min_score) {
                min_score = D[pp][j];
            }
        }
        Q[pp][i + diagonal] = PQD_INIT_VAL;
        D[pp][i + diagonal] = PQD_INIT_VAL;
        if (min_score > max_score) {
            return min_score;
        }
    }
    int min_score = PQD_INIT_VAL;
    for (int i = matrix_size - diagonal; i < matrix_size; i++) {
        int pp = i % 2;
        int lp = (i - 1) % 2;
        P[pp][i - diagonal] = PQD_INIT_VAL;
        D[pp][i - diagonal] = PQD_INIT_VAL;
        for (int j = i + 1 - diagonal; j < matrix_size; j++) {
            ODPD_compute(
                i, j, s1, s2, &P[pp][j], P[pp][j - 1], &Q[pp][j], Q[lp][j], &D[pp][j], D[pp][j - 1], D[lp][j], D[lp][j - 1]);
        }
        if (D[pp][matrix_size - 1] < min_score)
            min_score = D[pp][matrix_size - 1];
    }
    int i = matrix_size - 1;
    int pp = i % 2;
    for (unsigned int j = i + 1 - diagonal; j < (unsigned int)matrix_size; j++) {
        if (D[pp][j] < min_score) {
            min_score = D[pp][j];
        }
    }

    return min_score;
}

static int translation_table[256] = { 0, 10, 10, 10, 10, 20, 20, 20, 10, 20, 20, 20, 10, 20, 20, 20, 10, 20, 20, 20, 20, 30, 30,
    30, 20, 30, 30, 30, 20, 30, 30, 30, 10, 20, 20, 20, 20, 30, 30, 30, 20, 30, 30, 30, 20, 30, 30, 30, 10, 20, 20, 20, 20, 30,
    30, 30, 20, 30, 30, 30, 20, 30, 30, 30, 10, 20, 20, 20, 20, 30, 30, 30, 20, 30, 30, 30, 20, 30, 30, 30, 20, 30, 30, 30, 30,
    40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 20, 30, 30, 30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 20, 30, 30, 30,
    30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 10, 20, 20, 20, 20, 30, 30, 30, 20, 30, 30, 30, 20, 30, 30, 30, 20, 30, 30,
    30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 20, 30, 30, 30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 20, 30,
    30, 30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 10, 20, 20, 20, 20, 30, 30, 30, 20, 30, 30, 30, 20, 30, 30, 30, 20,
    30, 30, 30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40, 20, 30, 30, 30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40,
    20, 30, 30, 30, 30, 40, 40, 40, 30, 40, 40, 40, 30, 40, 40, 40 };

/**
 * @brief Optimized version of ODPD (if no INDELS)
 * If it detects INDELS, return -1. In this case we will need the run the full ODPD.
 */
static int noDP(int8_t *s1, int8_t *s2, int max_score)
{
    int score = 0;
    int size_neighbour = SIZE_NEIGHBOUR_IN_BYTES;
    for (int i = 0; i < size_neighbour - delta_neighbour; i++) {
        int s_xor = ((int)(s1[i] ^ s2[i])) & 0xFF;
        int s_translated = translation_table[s_xor];
        if (s_translated > COST_SUB) {
            int j = i + 1;
            /* INDELS detection */
            if (j < size_neighbour - delta_neighbour - 3) {
                int s1_val = ((int *)(&s1[j]))[0];
                int s2_val = ((int *)(&s2[j]))[0];
                if (((s1_val ^ (s2_val >> 2)) & 0x3FFFFFFF) == 0) {
                    return -1;
                }
                if (((s1_val ^ (s2_val >> 4)) & 0xFFFFFFF) == 0) {
                    return -1;
                }
                if (((s1_val ^ (s2_val >> 6)) & 0x3FFFFFF) == 0) {
                    return -1;
                }
                if (((s1_val ^ (s2_val >> 8)) & 0xFFFFFF) == 0) {
                    return -1;
                }
                if (((s2_val ^ (s1_val >> 2)) & 0x3FFFFFFF) == 0) {
                    return -1;
                }
                if (((s2_val ^ (s1_val >> 4)) & 0xFFFFFFF) == 0) {
                    return -1;
                }
                if (((s2_val ^ (s1_val >> 6)) & 0x3FFFFFF) == 0) {
                    return -1;
                }
                if (((s2_val ^ (s1_val >> 8)) & 0xFFFFFF) == 0) {
                    return -1;
                }
            }
        }
        score += s_translated;
        if (score > max_score)
            break;
    }
    return score;
}

static void align_on_dpu(unsigned int dpu_offset, unsigned rank_id, int pass_id)
{
    int nb_map = 0;
    int numdpu = dpu_offset + rank_id;
    if (numdpu >= (int)index_get_nb_dpu())
        return;
    int size_neighbour_in_symbols = SIZE_IN_SYMBOLS(delta_neighbour);
    dispatch_request_t *requests = dispatch_get(numdpu, pass_id);
    acc_results_t *acc_res = accumulate_get_buffer(rank_id, pass_id);

    for (unsigned int each_request_read = 0; each_request_read < requests->nb_reads; each_request_read++) {
        dpu_request_t *curr_request = &(requests->dpu_requests[each_request_read]);
        int min = MAX_SCORE;
        int nb_map_start = nb_map;
        int8_t *curr_read = (int8_t *)&curr_request->nbr[0];
        for (unsigned int nb_neighbour = 0; nb_neighbour < curr_request->count; nb_neighbour++) {
            coords_and_nbr_t *coord_and_nbr = &(mrams[rank_id][curr_request->offset + nb_neighbour]);
            int8_t *curr_nbr = (int8_t *)&coord_and_nbr->nbr[0];

            int score = noDP(curr_read, curr_nbr, min);
            if (score == -1) {
                score = ODPD(curr_read, curr_nbr, min, size_neighbour_in_symbols);
            }
            if (score > min)
                continue;

            if (score < min) {
                min = score;
                nb_map = nb_map_start;
            }

            if (nb_map >= MAX_DPU_RESULTS - 1) {
                ERROR_EXIT(ERR_SIMU_MAX_RESULTS_REACHED, "%s:[P%u, DPU#%u]: MAX_DPU_RESULTS reached!", __func__, pass_id, numdpu);
            }

            dpu_result_out_t *result = &acc_res->results[nb_map++];
            result->num = curr_request->num;
            result->coord = coord_and_nbr->coord;
            result->score = score;
        }
    }

    acc_res->results[nb_map].num = -1;
    acc_res->nb_res = nb_map;
}

static void *align_on_dpu_fct(void *arg)
{
    const unsigned int dpu_id = (unsigned int)(uintptr_t)arg;

    pthread_barrier_wait(&barrier);
    while (!stop_threads) {
        align_on_dpu(dpu_offset_shared, dpu_id, pass_id_shared);
        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}

void run_dpu_simulation(unsigned int dpu_offset, unsigned int pass_id, sem_t *dispatch_free_sem, sem_t *acc_wait_sem,
    sem_t *exec_to_acc_sem, sem_t *dispatch_to_exec_sem)
{
    sem_wait(acc_wait_sem);

    dpu_offset_shared = dpu_offset;
    pass_id_shared = pass_id;

    pthread_barrier_wait(&barrier);
    pthread_barrier_wait(&barrier);

    sem_post(dispatch_free_sem);
    sem_post(exec_to_acc_sem);
    sem_wait(dispatch_to_exec_sem);
}

void init_backend_simulation(unsigned int *nb_dpus_per_run)
{
    unsigned int nb_thread_for_simu = get_nb_thread_for_simu();
    *nb_dpus_per_run = nb_thread_for_simu;
    mrams = (coords_and_nbr_t **)calloc(nb_thread_for_simu, sizeof(coords_and_nbr_t *));
    assert(mrams != NULL);

    tids = malloc(nb_thread_for_simu * sizeof(pthread_t));
    assert(tids != NULL);

    assert(pthread_barrier_init(&barrier, NULL, nb_thread_for_simu + 1) == 0);

    FOREACH_THREAD(each_dpu)
    {
        int ret = pthread_create(&tids[each_dpu], NULL, align_on_dpu_fct, (void *)(uintptr_t)each_dpu);
        assert(ret == 0);
    }
}

void free_backend_simulation()
{
    stop_threads = true;
    pthread_barrier_wait(&barrier);
    assert(pthread_barrier_destroy(&barrier) == 0);

    FOREACH_THREAD(each_dpu)
    {
        int ret = pthread_join(tids[each_dpu], NULL);
        assert(ret == 0);
        free(mrams[each_dpu]);
    }

    free(tids);
    free(mrams);
}

void load_mram_simulation(unsigned int dpu_offset, __attribute__((unused)) int _delta_neighbour)
{
    FOREACH_THREAD(each_dpu)
    {
        unsigned int dpu_id = dpu_offset + each_dpu;
        if (dpu_id >= index_get_nb_dpu())
            return;
        free(mrams[each_dpu]);
        mram_load((uint8_t **)&mrams[each_dpu], dpu_id);
    }
}

void wait_dpu_simulation() { return; }
