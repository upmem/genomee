/**
 * @Copyright (c) 2016-2018 - Dominique Lavenier & UPMEM
 */

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "upvc.h"
#include "mram_dpu.h"
#include "dpus_mgmt.h"
#include "dpulog.h"
#include "dispatch.h"
#include "parse_args.h"

/* #define LOG_DPUS */
#ifdef LOG_DPUS
static dpu_logging_config_t logging_config = {
                                              .source = KTRACE,
                                              .destination_directory_name = "."
};
#define p_logging_config &logging_config
#define log_dpu(dpu, out) dpulog_read_for_dpu(dpu, out)
#else
#define p_logging_config NULL
#define log_dpu(dpu, log) do {} while(0)
#endif /* LOG_DPUS */

static struct dpu_param param = {
                                 .type = FUNCTIONAL_SIMULATOR,
                                 .profile = "",
                                 .on_boot = NULL,
                                 .logging_config = p_logging_config
};

void setup_dpus_for_target_type(target_type_t target_type)
{
        switch (target_type) {
        case target_type_fpga:
                param.type = HW;
                break;
        default:
                param.type = FUNCTIONAL_SIMULATOR;
                break;
        }
}

devices_t *dpu_try_alloc_for(unsigned int nb_dpus, const char *opt_program)
{
        devices_t *devices = (devices_t *) malloc(sizeof(devices_t));
        if (devices == NULL) {
                ERROR_EXIT(3, "*** could not allocate structure for devices - aborting");
        }
        devices->nb_dpus = nb_dpus;
        if (dpu_get_nr_of_dpus_for(&param, &(devices->nb_dpus_per_rank)) != DPU_API_SUCCESS) {
                ERROR_EXIT(30, "*** could not guess the number of DPUs per rank - aborting");
        }
        devices->dpus = (dpu_t *) calloc(nb_dpus, sizeof(dpu_t));
        if (devices->dpus == NULL) {
                ERROR_EXIT(4, "*** could not allocate array of DPUs - aborting");
        }
        unsigned int nb_ranks = (nb_dpus + devices->nb_dpus_per_rank - 1) / devices->nb_dpus_per_rank;
        devices->nb_ranks = nb_ranks;
        devices->ranks = (dpu_rank_t *) calloc(nb_ranks, sizeof(dpu_rank_t));
        if (devices->ranks == NULL) {
                ERROR_EXIT(5, "*** could not allocate array of DPU ranks - aborting");
        }

        for (unsigned int each_rank = 0, each_dpu = 0; each_dpu < nb_dpus; each_rank++) {
                if (dpu_alloc(&param, &(devices->ranks[each_rank])) != DPU_API_SUCCESS) {
                        ERROR_EXIT(6, "*** could not allocate rank number %u - aborting", each_rank);
                }
                for (unsigned int each_member = 0;
                     (each_member < devices->nb_dpus_per_rank) && (each_dpu < nb_dpus);
                     each_member++, each_dpu++) {
                        devices->dpus[each_dpu] = dpu_get_id(devices->ranks[each_rank], each_member);
                }
        }

        for (unsigned int each_rank = 0; each_rank < devices->nb_ranks; each_rank++) {
                if (dpu_load_all(devices->ranks[each_rank], opt_program) != DPU_API_SUCCESS) {
                        ERROR_EXIT(7, "*** could not load program on rank number %u - aborting", each_rank);
                }
        }

        return devices;
}

void dpu_try_write_mram(dpu_t dpu_id, devices_t *devices, mram_info_t *mram)
{
        dpu_copy_to_individual(devices->dpus[dpu_id],
                               (uint8_t *) mram,
                               MRAM_INFO_ADDR,
                               sizeof(mram_info_t) + mram->total_nbr_size);
}

void dpu_try_free(devices_t *devices)
{
        for (unsigned int each_rank = 0; each_rank < devices->nb_ranks; each_rank++) {
                dpu_free(devices->ranks[each_rank]);
        }
        free(devices->ranks);
        free(devices->dpus);
        free(devices);
}

void dpu_try_run(unsigned int dpu_id, devices_t *devices)
{
        if (dpu_boot_individual(devices->dpus[dpu_id], ASYNCHRONOUS) != DPU_API_SUCCESS) {
                log_dpu(devices->dpus[dpu_id], stdout);
                ERROR_EXIT(8, "*** run failed on DPU number %u - aborting!", dpu_id);
        }
}

bool dpu_try_check_status(unsigned int dpu_id, devices_t *devices)
{
        dpu_run_status_t run_status;
        if (dpu_get_individual_status(devices->dpus[dpu_id], &run_status) != DPU_API_SUCCESS) {
                ERROR_EXIT(9, "*** could not get status from DPU number %u - aborting", dpu_id);
        }

        switch (run_status) {
        case DPU_STATUS_IDLE:
                return true;
        case DPU_STATUS_RUNNING:
                return false;
        case DPU_STATUS_ERROR:
                log_dpu(devices->dpus[dpu_id], stdout);
                ERROR_EXIT(10, "*** DPU %u reported an error - aborting", dpu_id);
        default:
                log_dpu(devices->dpus[dpu_id], stdout);
                ERROR_EXIT(11, "*** could not get DPU status %u - aborting", dpu_id);
        }
}

void dpu_try_write_dispatch_into_mram(unsigned int dpu_id,
                                      devices_t *devices,
                                      unsigned int nb_reads,
                                      int8_t *reads,
                                      mram_info_t *mram,
                                      reads_info_t *reads_info)
{
        unsigned int io_len;
        request_info_t io_header =
                {
                 .nb_reads = nb_reads,
                 .magic = 0xcdefabcd
                };

        io_len = nb_reads * DPU_REQUEST_SIZE(reads_info->size_neighbour_in_bytes);

        if ((DPU_REQUEST_ADDR(mram) - DPU_INPUTS_ADDR + io_len) > DPU_INPUTS_SIZE) {
                ERROR_EXIT(12, "*** will exceed MRAM limit if writing reads on DPU number %u - aborting!", dpu_id);
        }

        dpu_copy_to_individual(devices->dpus[dpu_id],
                               (uint8_t *) &io_header,
                               (mram_addr_t) DPU_REQUEST_INFO_ADDR(mram),
                               sizeof(request_info_t));
        dpu_copy_to_individual(devices->dpus[dpu_id],
                               (uint8_t *) reads,
                               (mram_addr_t) DPU_REQUEST_ADDR(mram),
                               io_len);
}

#define CLOCK_PER_SEC (600000000.0)
void dpu_try_log(unsigned int dpu_id, devices_t *devices)
{
        dpu_compute_time_t compute_time;
        dpu_copy_from_individual(devices->dpus[dpu_id],
                                 (mram_addr_t) (DPU_COMPUTE_TIME_ADDR),
                                 (uint8_t *) (&compute_time),
                                 sizeof(dpu_compute_time_t));

        printf("LOG DPU=%u TIME=%llu SEC=%.3f\n", dpu_id, (unsigned long long)compute_time, (float)compute_time / CLOCK_PER_SEC);
        fflush(stdout);

#ifdef STATS_ON
        /* Collect stats */
        for (unsigned int each_tasklet = 0; each_tasklet < NB_TASKLET_PER_DPU; each_tasklet++) {
                dpu_tasklet_stats_t tasklet_stats;

                dpu_copy_from_individual(devices->dpus[dpu_id],
                                         (mram_addr_t) (DPU_TASKLET_STATS_ADDR + each_tasklet * sizeof(dpu_tasklet_stats_t)),
                                         (uint8_t *) (&tasklet_stats),
                                         sizeof(dpu_tasklet_stats_t));

                /* TODO: show logs */
                printf("LOG DPU=%u TID=%u REQ=%u\n", dpu_id, each_tasklet, tasklet_stats.nb_reqs);
                printf("LOG DPU=%u TID=%u NODP=%u\n", dpu_id, each_tasklet, tasklet_stats.nb_nodp_calls);
                printf("LOG DPU=%u TID=%u ODPD=%u\n", dpu_id, each_tasklet, tasklet_stats.nb_odpd_calls);
                printf("LOG DPU=%u TID=%u RESULTS=%u\n", dpu_id, each_tasklet, tasklet_stats.nb_results);
                printf("LOG DPU=%u TID=%u DATA_IN=%u\n", dpu_id, each_tasklet, tasklet_stats.mram_data_load);
                printf("LOG DPU=%u TID=%u RESULT_OUT=%u\n", dpu_id, each_tasklet, tasklet_stats.mram_result_store);
                printf("LOG DPU=%u TID=%u LOAD=%u\n", dpu_id, each_tasklet, tasklet_stats.mram_load);
                printf("LOG DPU=%u TID=%u STORE=%u\n", dpu_id, each_tasklet, tasklet_stats.mram_store);
        }
        fflush(stdout);
#endif

        log_dpu(devices->dpus[dpu_id], stdout);
}

void dpu_try_get_results(unsigned int dpu_id, devices_t *devices, dpu_result_out_t *result_buffer)
{
        dpu_copy_from_individual(devices->dpus[dpu_id],
                                 (mram_addr_t) DPU_RESULT_ADDR,
                                 (uint8_t *) result_buffer,
                                 DPU_RESULT_SIZE);
}

void dpu_try_backup_mram(unsigned int tid, devices_t *devices, const char *file_name)
{
        FILE *f = fopen(file_name, "wb");
        printf("saving DPU %u MRAM into '%s'\n", tid, file_name);
        if (f != NULL) {
                uint8_t *mram = (uint8_t *) malloc(MRAM_SIZE);
                dpu_copy_from_individual(devices->dpus[tid], 0, mram, MRAM_SIZE);
                fwrite(mram, 1, MRAM_SIZE, f);
                free(mram);
                fclose(f);
        } else {
                WARNING("failed to backup DPU %u MRAM into '%s'\n", tid, file_name);
        }
}
