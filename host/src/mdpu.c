/**
 * @Copyright (c) 2016-2018 - Dominique Lavenier & UPMEM
 */

#include <string.h>
#include <stdlib.h>

#include "mdpu.h"
#include "vmi.h"
#include "mdpu.h"
#include "upvc.h"

#define MRAM_FILE_NAME_SIZE (24)

mram_info_t *mram_create(reads_info_t *reads_info)
{
        mram_info_t *result = (mram_info_t *) malloc(MRAM_SIZE);
        if (result == NULL) {
                ERROR_EXIT(17, "*** malloc failed to create a new MRAM image!");
        }
        mram_reset(result, reads_info);
        return result;
}

void mram_free(mram_info_t *mram)
{
        free(mram);
}

void mram_reset(mram_info_t *mram, reads_info_t *reads_info)
{
        mram->delta = 0;
        mram->total_nbr_size = 0;
        mram->nb_nbr = 0;
        mram->nbr_len = (uint32_t) reads_info->size_neighbour_in_bytes;
}

bool mram_copy_vmi(mram_info_t *mram, vmi_t *vmi, unsigned int nb_nbr, reads_info_t *reads_info)
{
        if (vmi->mem_size + sizeof(mram_info_t) >= MRAM_SIZE) {
                ERROR_EXIT(18, "MRAM size exceeded when copying %u bytes of neighbourhood", (unsigned int) vmi->mem_size);
        }

        mram_reset(mram, reads_info);
        mram->nb_nbr = nb_nbr;

        vmi_read(vmi, ((uint8_t *) mram) + ALIGN_DPU(sizeof(mram_info_t)), vmi->mem_size);
        mram->total_nbr_size = ALIGN_DPU(vmi->mem_size);
        return true;
}


static void make_mram_file_name(unsigned int dpu_id, char *name)
{
        sprintf(name, "mram_%04u.bin", dpu_id);
}

bool mram_save(mram_info_t *mram, unsigned int dpu_id)
{
        char file_name[MRAM_FILE_NAME_SIZE];
        make_mram_file_name(dpu_id, file_name);
        FILE *f = fopen(file_name, "wb");
        if (f == NULL) {
                ERROR_EXIT(19, "FATAL: could not create file '%s' for writing - aborting!", file_name);
        }

        size_t write_size = ALIGN_DPU(mram->total_nbr_size + sizeof(mram_info_t));
        size_t written = fwrite(mram, sizeof(uint8_t), write_size, f);
        if (written != write_size) {
                ERROR_EXIT(20, "BUG! fwrite in mram_save (%s) returned %lu instead of %lu - aborting!", file_name, written,
                           write_size);
        }

        fclose(f);
        return true;
}

void mram_load(mram_info_t *mram, unsigned int dpu_id)
{
        char file_name[MRAM_FILE_NAME_SIZE];
        make_mram_file_name(dpu_id, file_name);
        FILE *f = fopen(file_name, "rb");
        if (f == NULL) {
                ERROR_EXIT(21, "could not load MRAM file '%s'", file_name);
        }

        fread(mram, sizeof(uint8_t), MRAM_SIZE, f);

        fclose(f);
}
