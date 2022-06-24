/**
 * Copyright 2016-2019 - Dominique Lavenier & UPMEM
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "genome.h"
#include "parse_args.h"
#include "upvc.h"
#include "vartree.h"

static variant_t **variant_list[MAX_SEQ_GEN] = { NULL };
static pthread_mutex_t mutex;

void variant_tree_init()
{
    genome_t *genome = genome_get();
    pthread_mutex_init(&mutex, NULL);
    for (unsigned int each_seq = 0; each_seq < genome->nb_seq; each_seq++) {
        variant_list[each_seq] = (variant_t **)calloc(genome->len_seq[each_seq], sizeof(variant_t *));
    }
}

void variant_tree_insert(variant_t *var, uint32_t seq_nr, uint32_t offset_in_chr)
{
    pthread_mutex_lock(&mutex);
    variant_t **entry = &variant_list[seq_nr][offset_in_chr];
    variant_t *vars = *entry;
    while (vars != NULL) {
        if (!strcmp(vars->ref, var->ref) && !strcmp(vars->alt, var->alt)) {
            vars->depth++;
            vars->score += var->score;
            free(var);
            goto end;
        }
        vars = vars->next;
    }
    var->next = *entry;
    *entry = var;

end:
    pthread_mutex_unlock(&mutex);
}

void variant_tree_free()
{
    genome_t *genome = genome_get();
    pthread_mutex_destroy(&mutex);
    for (unsigned int each_seq = 0; each_seq < genome->nb_seq; each_seq++) {
        for (unsigned int i = 0; i < genome->len_seq[each_seq]; i++) {
            variant_t *tmp = variant_list[each_seq][i];
            while (tmp != NULL) {
                variant_t *to_free = tmp;
                tmp = tmp->next;
                free(to_free);
            }
        }
        free(variant_list[each_seq]);
    }
}

typedef struct {
    uint32_t percentage;
    uint32_t score;
} depth_filter_t;

#if (SIZE_READ == 120)
depth_filter_t sub_filter[] = {
    [3] = { 15, 16 },
    [4] = { 17, 17 },
    [5] = { 18, 18 },
    [6] = { 20, 18 },
    [7] = { 21, 20 },
    [8] = { 22, 21 },
    [9] = { 22, 21 },
    [10] = { 24, 21 },
    [11] = { 24, 21 },
    [12] = { 28, 21 },
    [13] = { 29, 22 },
    [14] = { 29, 23 },
    [15] = { 32, 24 },
    [16] = { 32, 25 },
    [17] = { 35, 25 },
    [18] = { 35, 25 },
    [19] = { 35, 25 },
    [20] = { 40, 25 },
};

depth_filter_t indel_filter[] = {
    [2] = { 10, 16 },
    [3] = { 12, 21 },
    [4] = { 13, 21 },
    [5] = { 14, 22 },
    [6] = { 14, 22 },
    [7] = { 1, 23 },
    [8] = { 1, 25 },
    [9] = { 1, 25 },
    [10] = { 1, 30 },
    [11] = { 1, 40 },
};
#elif (SIZE_READ == 150)
depth_filter_t sub_filter[] = {
    [3] = { 15, 16 },
    [4] = { 17, 20 },
    [5] = { 18, 20 },
    [6] = { 20, 21 },
    [7] = { 21, 21 },
    [8] = { 22, 21 },
    [9] = { 24, 22 },
    [10] = { 25, 23 },
    [11] = { 27, 23 },
    [12] = { 27, 25 },
    [13] = { 29, 25 },
    [14] = { 30, 27 },
    [15] = { 31, 27 },
    [16] = { 34, 27 },
    [17] = { 34, 27 },
    [18] = { 34, 29 },
    [19] = { 35, 29 },
    [20] = { 40, 29 },
};

depth_filter_t indel_filter[] = {
    [2] = { 9, 21 },
    [3] = { 12, 22 },
    [4] = { 12, 22 },
    [5] = { 13, 24 },
    [6] = { 15, 25 },
    [7] = { 17, 25 },
    [8] = { 18, 25 },
    [9] = { 2, 26 },
    [10] = { 1, 27 },
    [11] = { 1, 40 },
};
#else
#error "Filter not defined for this size of read"
#endif

static bool homopolymer(int8_t *seq, int offset)
{
    for (int i = 0; i < offset - 1; i++) {
        if (seq[i] != seq[i + 1]) {
            return false;
        }
    }
    return true;
}

static bool print_variant_tree(variant_t *var, uint32_t seq_nr, uint64_t seq_pos, genome_t *ref_genome, FILE *vcf_file)
{
    char *chr = ref_genome->seq_name[seq_nr];
    uint64_t genome_pos = ref_genome->pt_seq[seq_nr] + seq_pos;
    uint32_t cov = ref_genome->mapping_coverage[genome_pos];
    uint32_t depth = var->depth;
    uint32_t score = var->score / depth;
    uint32_t percentage = 100;
    if (cov != 0) {
        percentage = depth * 100 / cov;
    }

    uint32_t ref_len = strlen(var->ref);
    uint32_t alt_len = strlen(var->alt);
    if (ref_len > alt_len && percentage <= 25 && homopolymer(&ref_genome->data[genome_pos - 12], 12)) {
        return false;
    }

    if (get_no_filter())
        goto print;

    if (ref_len == alt_len) { /* SUBSTITUTION */
        if (depth < 3) {
            return false;
        } else if (depth > 20) {
            depth = 20;
        }
        if (!(score <= sub_filter[depth].score && percentage >= sub_filter[depth].percentage)) {
            return false;
        }
    } else { /* INSERTION OR DELETION */
        if (depth < 2) {
            return false;
        } else if (depth > 11) {
            depth = 11;
        }
        if (!(score <= indel_filter[depth].score && percentage >= indel_filter[depth].percentage)) {
            return false;
        }
    }

print:
    fprintf(vcf_file, "%s\t%lu\t.\t%s\t%s\t.\t.\tDEPTH=%d;COV=%d;SCORE=%d\n", chr, seq_pos, var->ref, var->alt, var->depth, cov,
        score);

    return true;
}

void create_vcf()
{
    double start_time = my_clock();
    printf("%s:\n", __func__);

    FILE *vcf_file;
    int nb_variant = 0;
    char filename[1024];
    genome_t *ref_genome = genome_get();

    sprintf(filename, "%s_upvc.vcf", get_input_path());
    vcf_file = fopen(filename, "w");
    CHECK_FILE(vcf_file, filename);

    /* ####### START OF HEADER ####### */

    /* print vcf version (required) */
    fprintf(vcf_file, "##fileformat=VCFv4.3\n");

    /* print source of VCF file (this program) */
    fprintf(vcf_file, "##source=UPVC %s\n", VERSION);

    /* get the file date */
    char filedate[10];
    time_t mytime = time(NULL);
    strftime(filedate, 100, "%Y%d%m", localtime(&mytime));
    fprintf(vcf_file, "##fileDate=%s\n", filedate);

    /* print reference genome file name */
    fprintf(vcf_file, "##reference=%s.fasta\n", get_input_path());

    /* print the column names (fields are tab-delimited in VCF) */
    fprintf(vcf_file, "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n");

    /* ####### END OF HEADER ####### */

    /* for each sequence in the genome */
    for (uint32_t seq_number = 0; seq_number < ref_genome->nb_seq; seq_number++) {
        /* for each position in the sequence */
        for (uint64_t seq_position = 0; seq_position < ref_genome->len_seq[seq_number]; seq_position++) {
            variant_t *var = variant_list[seq_number][seq_position];
            while (var != NULL) {
                nb_variant += print_variant_tree(var, seq_number, seq_position, ref_genome, vcf_file) ? 1 : 0;
                var = var->next;
            }
        }
    }

    fclose(vcf_file);

    printf("\tnumber of variants: %d\n", nb_variant);
    printf("\ttime: %lf s\n", my_clock() - start_time);
}
