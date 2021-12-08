/**
 * Copyright 2021 - A. Moisson-Franckhauser & UPMEM
 */

#include <stdlib.h>
#include <errno.h>

#include "index.h"
#include "mapping_file.h"
#include "genome.h"
#include "common.h"
#include "parse_args.h"
#include "processread.h"
#include "debug.h"

#define MAP_FILENAME "read_alignments.map"
#define MAP_VERSION "1.6"
#define PROGRAM_NAME "upvc"
#define MAX_CIGAR_LENGTH (3*SIZE_READ)
#define MAX_PATCH_LENGTH (3*SIZE_READ)

#define CIGAR_MATCH '='
#define CIGAR_MISMATCH 'X'
#define CIGAR_INSERT 'I'
#define CIGAR_DELETE 'D'

FILE *mapping_file;

static char *get_mapping_filename()
{
    static char filename[FILENAME_MAX];
    sprintf(filename, "%s", MAP_FILENAME);
    LOG_TRACE("mapping filename : \"%s\"\n", filename);
    return filename;
}

void open_mapping_file()
{
    LOG_DEBUG("opening mapping file\n");
    // TODO: check for memory leaks here
    char *filename = get_mapping_filename();
    mapping_file = fopen(filename, "w");
    if (mapping_file == NULL)
    {
        LOG_FATAL("couldn't open mapping file; errno : %u\n", errno);
    }
    LOG_DEBUG("openned mapping file : %p\n", mapping_file);
    // TODO: complete header
    LOG_TRACE("writing mapping header\n");
    //fprintf(mapping_file, "@HD VN:" MAP_VERSION " SO:unknown\n");
    //fprintf(mapping_file, "@PG ID:1 PN:" PROGRAM_NAME "\n");
    //LOG_DEBUG("mapping header written\n");
}

/*
 * TODO: Either reuse this code or delete it
void write_mapping_read(uint64_t genome_pos, uint8_t *code, int8_t *read)
{
    const unsigned int flag = 0x0;
    const uint8_t mapping_quality = 255;// unknown quality; TODO: set quality

    char cigar[MAX_CIGAR_LENGTH];
    uint32_t cigar_idx = 0;
    char last_code = 0;
    unsigned int code_count = 0;
    int last_position = -1;
    int tlen = 0;//TODO: figure out what tlen is supposed to be

    char sequence[SIZE_READ+1];
    char nucleotide[4] = {'A', 'C', 'T', 'G'};

    for (uint32_t code_idx=0; code[code_idx] != CODE_END;)
    {
        char new_code=0;
        int new_position=last_position+1;
        switch (code[code_idx++])
        {
            case CODE_SUB:
                new_code = CIGAR_MISMATCH;
                new_position = code[code_idx++];
                code_idx++;
                break;
            case CODE_INS:
                new_code = CIGAR_INSERT;
                new_position = code[code_idx++];
                code_idx++;
                break;
            case CODE_DEL:
                new_code = CIGAR_DELETE;
                new_position = code[code_idx++];
                code_idx++;
                break;
        }
        if (last_code != new_code)
        {
            if (code_count>0) 
            {
                cigar_idx += sprintf(cigar+cigar_idx, "%u%c", code_count, last_code);
            }
            last_code = new_code;
            code_count = 0;
        }
        if (new_position > last_position+1) 
        {
            if (code_count > 0)
            {
                cigar_idx += sprintf(cigar+cigar_idx, "%u%c", code_count, last_code);
                code_count = 0;
            }
            cigar_idx += sprintf(cigar+cigar_idx, "%u%c", new_position-last_position-1, CIGAR_MATCH);
        }
        code_count++;
        last_position = new_position;
    }
    if (code_count > 0)
    {
        cigar_idx += sprintf(cigar+cigar_idx, "%u%c", code_count, last_code);
    }
    if (SIZE_READ > last_position+1)
    {
        cigar_idx += sprintf(cigar+cigar_idx, "%u%c", SIZE_READ-last_position-1, CIGAR_MATCH);
    }
    cigar[cigar_idx] = 0;//ensure last string character is 0
    LOG_TRACE("cigar : \"%s\"\n", cigar);

    for (uint32_t i=0; i<SIZE_READ; i++)
    {
        sequence[i] = nucleotide[read[i]];
    }
    sequence[SIZE_READ] = 0;

    //TODO: set read quality correctly (last string)
    fprintf(mapping_file, "*\t%u\t*\t%lu\t%u\t%s\t*\t0\t%d\t%s\t*\n", flag, genome_pos+1, mapping_quality, cigar, tlen, sequence);
}
*/

void write_read_mapping(char *chromosome_name, uint64_t genome_pos, uint8_t *code, uint8_t *read) {
    char patch[MAX_PATCH_LENGTH];
    int patch_idx=0;
    int read_idx=0;

    uint32_t code_idx;
    char nucleotide[4]= {'A', 'C', 'T', 'G'};
    uint8_t last_action = CODE_INS;
    for (code_idx=0; code[code_idx] < CODE_END;)
    {
        uint8_t action = code[code_idx++];
        uint8_t position = read_idx;
        uint8_t letter='E';
        if (action>3) 
        {
            position = code[code_idx++];
            if (code[code_idx] < 5)
            {
                letter = nucleotide[code[code_idx++]];
            } else {
                letter = nucleotide[(code[code_idx++] && 0x6)>>1];
            }
        }
        for (;read_idx<position;)
        {
            if (read[read_idx]<5)
            {
                patch[patch_idx++] = nucleotide[read[read_idx++]]|0x20;//lowercase
            } else {
                patch[patch_idx++] = nucleotide[(read[read_idx++] && 0x6)>>1]|0x20;//lowercase
            }
            // patch[patch_idx++] = '=';
        }
        switch (action)
        {
            case CODE_SUB:
                patch[patch_idx++] = letter|0x20;//lowercase
                read_idx++;
                break;
            case CODE_DEL:
                patch[patch_idx++] = '/';
                break;
            case CODE_INS:
                patch[patch_idx++] = letter;
                read_idx++;
                break;
            default:
                //Consider the code read is not an action but a letter associated with the previous action
                if (action < 5)
                {
                    letter = nucleotide[action];
                } else {
                    letter = nucleotide[(action && 0x6) >> 1];
                }
                switch (last_action)
                {
                    case CODE_SUB:
                        patch[patch_idx++] = letter|0x20;//lowercase
                        read_idx++;
                        break;
                    case CODE_DEL:
                        patch[patch_idx++] = '/';
                        break;
                    case CODE_INS:
                        patch[patch_idx++] = letter;
                        read_idx++;
                        break;
                }
        }
    }
    if (code[code_idx] == CODE_ERR)
    {
        LOG_ERROR("found CODE_ERR in read code\n");
        return;
    }
    if (code[code_idx] != CODE_END)
    {
        LOG_ERROR("found unsuspected code : %u\n", code[code_idx]);
        for (uint32_t i = 0; i<code_idx; i++) {
            LOG_TRACE("code[%u]=%u\n", i, code[i]);
        }
    }
    for (;read_idx<SIZE_READ;)
    {
        if (read[read_idx]<5)
        {
            patch[patch_idx++] = nucleotide[read[read_idx++]]|0x20;//lowercase
        } else {
            patch[patch_idx++] = nucleotide[(read[read_idx++] && 0x6)>>1]|0x20;//lowercase
        }
        // patch[patch_idx++] = '=';
    }
    patch[patch_idx++] = 0&&code;
    fprintf(mapping_file, "%s\t%lu\t%s\n", chromosome_name, genome_pos, patch);
}

void close_mapping_file()
{
    LOG_TRACE("closing mapping file\n");
    fclose(mapping_file);
}
