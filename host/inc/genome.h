/**
 * Copyright 2016-2019 - Dominique Lavenier & UPMEM
 */

#ifndef __GENOME_H__
#define __GENOME_H__

#include <stdint.h>

#define MAX_SEQ_GEN (24) // max number of chromosomes
#define MAX_SEQ_NAME_SIZE (8)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t nb_seq;
    uint64_t pt_seq[MAX_SEQ_GEN];
    uint64_t len_seq[MAX_SEQ_GEN];
    uint64_t fasta_file_size;
    char seq_name[MAX_SEQ_GEN][MAX_SEQ_NAME_SIZE];
    int8_t *data;
    int32_t *mapping_coverage;
} genome_t;

void genome_create();

void genome_load();

void genome_free();

genome_t *genome_get();

#endif /* __GENOME_H__ */
