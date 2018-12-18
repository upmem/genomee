/**
 * @Copyright (c) 2016-2018 - Dominique Lavenier & UPMEM
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "parse_args.h"
#include "dispatch.h"
#include "getread.h"
#include "index.h"
#include "processread.h"
#include "vartree.h"
#include "vcf.h"
#include "genome.h"
#include "upvc_dpu.h"
#include "upvc.h"
#include "mram_dpu.h"
#include "backends_functions.h"
#include "simu_backend.h"
#include "dpu_backend.h"

static void run_pass(int round,
                     int nb_read,
                     int nb_read_total,
                     unsigned int dpu_offset,
                     int nb_pass,
                     FILE *fope1,
                     FILE *fope2,
                     int8_t *reads_buffer,
                     dpu_result_out_t *result_tab,
                     genome_t *ref_genome,
                     index_seed_t **index_seed,
                     dispatch_request_t *dispatch_requests,
                     variant_tree_t **variant_list,
                     int *substitution_list,
                     int8_t *mapping_coverage,
                     devices_t *devices,
                     reads_info_t *reads_info,
                     times_ctx_t *times_ctx,
                     backends_functions_t *backends_functions)
{
        int nb_read_map;
        printf("Round %d / DPU offset %d / Pass %d\n", round, dpu_offset, nb_pass);
        printf(" - get %d reads (%d)\n", nb_read / 2, nb_read_total / 2);
        printf(" - time to get reads      : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->get_reads,
               times_ctx->tot_get_reads);

        if (DEBUG_PASS != -1 && DEBUG_PASS != nb_pass) {
                return;
        }

        dispatch_read(index_seed,
                      reads_buffer,
                      nb_read,
                      dispatch_requests,
                      times_ctx,
                      reads_info,
                      backends_functions);
        printf(" - time to dispatch reads : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->dispatch_read,
               times_ctx->tot_dispatch_read);
        PRINT_TIME(times_ctx, "%lf, , , %f, %f, , , %f\n", my_clock(), nb_pass + 0.2, nb_pass + 0.3, nb_pass + 0.6);

        backends_functions->run_dpu(dispatch_requests,
                                    devices,
                                    dpu_offset,
                                    nb_pass,
                                    times_ctx,
                                    reads_info);
        printf(" - time to write reads      : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->write_reads,
               times_ctx->tot_write_reads);
        printf(" - time to compute          : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->compute,
               times_ctx->tot_compute);
        printf(" - time to read results     : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->read_result,
               times_ctx->tot_read_result);
        printf(" - time to map reads        : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->map_read,
               times_ctx->tot_map_read);

        PRINT_TIME(times_ctx, "%lf, , , , , , %f, %f, %f\n", my_clock(), nb_pass + 0.5, nb_pass + 0.6, nb_pass + 0.7);

        if (DEBUG_PASS != -1) {
                return;
        }

        nb_read_map = process_read(ref_genome,
                                   reads_buffer,
                                   variant_list,
                                   substitution_list,
                                   mapping_coverage,
                                   result_tab,
                                   fope1,
                                   fope2,
                                   round,
                                   dpu_offset,
                                   times_ctx,
                                   reads_info);
        printf(" - time to process reads  : %7.2lf sec. / %7.2lf sec.\n",
               times_ctx->process_read,
               times_ctx->tot_process_read);
        printf(" - map %i reads\n", nb_read_map);
        printf("\n");
}

static void map_var_call(char *filename_prefix,
                         int round,
                         devices_t *devices,
                         genome_t *ref_genome,
                         index_seed_t **index_seed,
                         dispatch_request_t *dispatch_requests,
                         variant_tree_t **variant_list,
                         int *substitution_list,
                         int8_t *mapping_coverage,
                         int8_t *reads_buffer,
                         dpu_result_out_t *result_tab,
                         reads_info_t *reads_info,
                         times_ctx_t *times_ctx,
                         backends_functions_t *backends_functions)
{
        char filename[1024];
        FILE *fope1, *fope2;  /* pair-end output file descriptor */
        unsigned int nb_dpu = get_nb_dpu();

        reads_info->delta_neighbour_in_bytes = (SIZE_SEED * round)/4;
        reads_info->size_neighbour_in_32bits_words =
                (reads_info->size_neighbour_in_bytes-reads_info->delta_neighbour_in_bytes) * 4;

        sprintf(filename, "%s_%d_PE1.fasta", filename_prefix, round+1);
        fope1 = fopen(filename, "w");
        sprintf(filename, "%s_%d_PE2.fasta", filename_prefix, round+1);
        fope2 = fopen(filename, "w");

        sprintf(filename, "%s_%d_time.csv", filename_prefix, round);
        times_ctx->time_file = fopen(filename, "w");
        fprintf(times_ctx->time_file, "time, write_mram, get_reads, dispatch_reads, write_reads, compute, read_result, map_read, process_read\n");

        /*
         * Loop:
         *   - Read a group of reads
         *   - Dispatch reads on DPUs
         *   - Execution on DPUs
         *   - Reads post-processing
         */
        for (unsigned int dpu_offset = 0; dpu_offset < nb_dpu; dpu_offset += get_nb_dpus_per_run()) {
                int nb_read;
                int nb_read_total = 0;
                int nb_pass = 0;
                FILE *fipe1, *fipe2;  /* pair-end input file descriptor */

                if (round == 0) {
                        sprintf(filename, "%s_PE1.fastq", filename_prefix);
                        fipe1 = fopen(filename, "r");
                        sprintf(filename, "%s_PE2.fastq", filename_prefix);
                        fipe2 = fopen(filename, "r");
                } else {
                        sprintf(filename, "%s_%d_PE1.fasta", filename_prefix, round);
                        fipe1 = fopen(filename, "r");
                        sprintf(filename, "%s_%d_PE2.fasta", filename_prefix, round);
                        fipe2 = fopen(filename, "r");
                }
                PRINT_TIME(times_ctx, "%lf, %f\n", my_clock(), nb_pass + 0.0);
                backends_functions->load_mram(dpu_offset, devices, reads_info, times_ctx);
                printf(" - time to write MRAMs : %7.2lf sec. / %7.2lf sec.\n",
                       times_ctx->write_mram,
                       times_ctx->tot_write_mram);

                PRINT_TIME(times_ctx, "%lf, %f, %f\n", my_clock(), nb_pass + 0.0, nb_pass + 0.1);

                nb_read = get_reads(fipe1, fipe2, reads_buffer, times_ctx, reads_info);

                PRINT_TIME(times_ctx, "%lf, , %f, %f\n", my_clock(), nb_pass + 0.1, nb_pass + 0.2);

                while ( nb_read != 0) {
                        nb_read_total += nb_read;
                        run_pass(round, nb_read, nb_read_total, dpu_offset, nb_pass,
                                 fope1, fope2,
                                 reads_buffer, result_tab,
                                 ref_genome,
                                 index_seed, dispatch_requests,
                                 variant_list, substitution_list, mapping_coverage,
                                 devices,
                                 reads_info, times_ctx, backends_functions);
                        nb_pass++;

                        PRINT_TIME(times_ctx, "%lf, , %f, , , , , , %f\n", my_clock(), nb_pass + 0.1, nb_pass-1 + 0.7);
                        nb_read = get_reads(fipe1, fipe2, reads_buffer, times_ctx, reads_info);
                        PRINT_TIME(times_ctx, "%lf, , %f, %f\n", my_clock(), nb_pass + 0.1, nb_pass + 0.2);
                }

                fclose(fipe1);
                fclose(fipe2);
        }

        fclose(times_ctx->time_file);
        fclose(fope1);
        fclose(fope2);
}

static void reload_and_verify_mram_images(reads_info_t *reads_info)
{
        index_seed_t **index_seed;
        FILE *seed_file;
        unsigned int nb_dpu = get_nb_dpu();
        malloc_dpu(reads_info, nb_dpu);
        index_seed = load_index_seeds();
        seed_file = fopen(SEED_FILE_LOG, "w");
        print_index_seeds(index_seed, seed_file, reads_info);
        fclose(seed_file);
        printf("Please check %s to verify that the indexing is OK\n", SEED_FILE_LOG);

        free_index(index_seed);
        free_dpu(nb_dpu);
}

static void load_index_save_genome(reads_info_t *reads_info, times_ctx_t *times_ctx, backends_functions_t *backends_functions)
{
        genome_t *ref_genome = get_genome(get_input_fasta(), times_ctx);
        index_seed_t **index_seed = index_genome(ref_genome,
                                                 get_nb_dpu(),
                                                 times_ctx,
                                                 reads_info,
                                                 backends_functions);
        save_index_seeds(index_seed);

        free_genome(ref_genome);
        free_index(index_seed);
        free_dpu(get_nb_dpu());
}

static void do_mapping(backends_functions_t *backends_functions, reads_info_t *reads_info, times_ctx_t *times_ctx)
{
        index_seed_t **index_seed;
        char *input_prefix = get_input_path();
        unsigned int nb_dpu = get_nb_dpu();
        variant_tree_t *variant_list = NULL;
        genome_t *ref_genome = get_genome(get_input_fasta(), times_ctx);
        devices_t *devices;

        int8_t *mapping_coverage = (int8_t *) calloc(sizeof(int8_t), ref_genome->fasta_file_size);
        int *substitution_list = (int *) calloc(sizeof(int), ref_genome->fasta_file_size);
        dpu_result_out_t *result_tab = (dpu_result_out_t *) malloc(sizeof(dpu_result_out_t) * MAX_DPU_RESULTS * nb_dpu);
        int8_t *reads_buffer = (int8_t *) malloc(sizeof(int8_t) * MAX_READS_BUFFER * reads_info->size_read);
        dispatch_request_t *dispatch_requests = dispatch_create(nb_dpu, reads_info);

        if ((DEBUG_NB_RUN != -1 && DEBUG_FIRST_RUN == -1)
            || (DEBUG_NB_RUN == 0)) {
                ERROR_EXIT(42, "DEBUG MACRO has not been well configured!");
        }

        backends_functions->init_backend(&devices,
                                         get_nb_dpus_per_run(),
                                         get_dpu_binary(),
                                         &index_seed,
                                         nb_dpu,
                                         ref_genome,
                                         reads_info,
                                         times_ctx,
                                         backends_functions);

        for (int round = 0; round < 3; round++) {
                if (DEBUG_ROUND != -1 && DEBUG_ROUND != round) {
                        continue;
                }
                map_var_call(input_prefix, round, devices, ref_genome, index_seed, dispatch_requests,
                             &variant_list, substitution_list, mapping_coverage, reads_buffer, result_tab,
                             reads_info, times_ctx, backends_functions);
        }

        backends_functions->free_backend(devices, nb_dpu);

        create_vcf(input_prefix, ref_genome, &variant_list, substitution_list, mapping_coverage, times_ctx);

        free_variant_tree(variant_list);
        free_genome(ref_genome);
        free_index(index_seed);
        dispatch_free(dispatch_requests, nb_dpu);
        free(reads_buffer);
        free(result_tab);
        free(substitution_list);
        free(mapping_coverage);
}

static void print_time()
{
        time_t timer;
        char time_buf[26];
        struct tm* tm_info;

        time(&timer);
        tm_info = localtime(&timer);

        strftime(time_buf, 26, "%Y-%m-%d %H:%M:%S", tm_info);
        printf("upvc started at: %s\n", time_buf);
}

int main(int argc, char *argv[])
{
        reads_info_t reads_info;
        times_ctx_t times_ctx;
        backends_functions_t backends_functions;

        memset(&times_ctx, 0, sizeof(times_ctx_t));

        validate_args(argc, argv);

        printf("%s\n", VERSION);
        print_time();

        reads_info.size_read = get_read_size(get_input_pe1());
        reads_info.size_neighbour_in_bytes = (reads_info.size_read - SIZE_SEED) / 4;
        printf("Information\n");
        printf(" - read size: %d\n", reads_info.size_read);

        setup_dpus_for_target_type(get_target_type());

        if (get_simulation_mode()) {
                backends_functions.init_backend = init_backend_simulation;
                backends_functions.free_backend = free_backend_simulation;
                backends_functions.run_dpu = run_dpu_simulation;
                backends_functions.add_seed_to_requests = add_seed_to_simulation_requests;
                backends_functions.init_vmis = init_vmis_simulation;
                backends_functions.free_vmis = free_vmis_simulation;
                backends_functions.write_vmi = write_vmi_simulation;
                backends_functions.load_mram = load_mram_simulation;
        } else {
                backends_functions.init_backend = init_backend_dpu;
                backends_functions.free_backend = free_backend_dpu;
                backends_functions.run_dpu = run_on_dpu;
                backends_functions.add_seed_to_requests = add_seed_to_dpu_requests;
                backends_functions.init_vmis = init_vmis_dpu;
                backends_functions.free_vmis = free_vmis_dpu;
                backends_functions.write_vmi = write_vmi_dpu;
                backends_functions.load_mram = load_mram_dpu;
        }

        switch(get_goal()) {
        case goal_index:
                load_index_save_genome(&reads_info, &times_ctx, &backends_functions);
                break;
        case goal_check:
                reload_and_verify_mram_images(&reads_info);
                break;
        case goal_map:
                do_mapping(&backends_functions, &reads_info, &times_ctx);
                break;
        case goal_unknown:
        default:
                ERROR_EXIT(23, "goal has not been specified!");
        }

        free_args();

        return 0;
}
