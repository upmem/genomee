#!/usr/bin/env python3

import sys
import re
import argparse
import os
try:
    import tqdm
    import_tqdm = True
except ImportError:
    import_tqdm = False


parser = argparse.ArgumentParser(
    description='Compute VCF quality compare to reference VCF')
parser.add_argument(
    "ref_file",
    metavar="<reference.vcf>",
    help="The reference VCF")
parser.add_argument(
    "upvc_file",
    metavar="<upvc.vcf>",
    help="The VCF generated by UPVC")
parser.add_argument(
    "-t",
    dest="tp_power",
    type=float,
    help="The power to use for TP during auto compute of UPVC filter",
    default=2.0)
parser.add_argument(
    "-a",
    dest="enable_stat",
    help="Enable auto compute of UPVC filter",
    action="store_true")
args = parser.parse_args()


def per(val, total):
    if total == 0:
        return -1
    return (val * 100.0) / total


###############################################################################
# AUTO COMPUTE UPVC FILTER ####################################################
###############################################################################


def compute_score(tp_var, fp_var, tp, tp_power):
    tp_per = (tp_var / (tp_var + fp_var)) if tp_var + fp_var != 0 else 0
    cm_per = (tp_var / tp) if tp != 0 else 0
    return (tp_per ** tp_power) * cm_per


def compute_best_filter_for_depth_per_score(
        best_filter, filter_score,
        nb_tp, nb_fp,
        percentage, score,
        tp_context, fp_context,
        tp, tp_power):
    tp_var = tp_context[(percentage + 1, score)] \
        + tp_context[(percentage, score - 1)] \
        - tp_context[(percentage + 1, score - 1)] \
        + nb_tp
    fp_var = fp_context[(percentage + 1, score)] \
        + fp_context[(percentage, score - 1)] \
        - fp_context[(percentage + 1, score - 1)] \
        + nb_fp
    tp_context[(percentage, score)] = tp_var
    fp_context[(percentage, score)] = fp_var

    new_score = compute_score(tp_var, fp_var, tp, tp_power)

    if new_score >= filter_score:
        return (percentage, score), new_score
    else:
        return best_filter, filter_score


def compute_best_filter_for_depth(tp_stat, fp_stat, tp, depth_filter, tp_power):
    best_filter = (100, 0)
    filter_score = compute_score(1, 1, 2, tp_power) # 50%tp/50%cm
    tp_context = {}
    fp_context = {}
    for score in range(9, 41):
        tp_context[(101, score)] = 0
        fp_context[(101, score)] = 0
    for percentage in range(100, 0, -1):
        tp_context[(percentage, 9)] = 0
        fp_context[(percentage, 9)] = 0
        for score in range(10, 41):
            best_filter, filter_score = compute_best_filter_for_depth_per_score(
                best_filter, filter_score,
                tp_stat[(percentage, score)] if (percentage, score) in tp_stat else 0,
                fp_stat[(percentage, score)] if (percentage, score) in fp_stat else 0,
                percentage, score,
                tp_context, fp_context,
                tp, tp_power)

    tp_filtered = sum([val for (per, sc), val in tp_stat.items()
                       if per >= best_filter[0] and sc <= best_filter[1]])
    fp_filtered = sum([val for (per, sc), val in fp_stat.items()
                       if per >= best_filter[0] and sc <= best_filter[1]])
    return best_filter, tp_filtered, fp_filtered


depth_limit = 21


def compute_best_filter(stat, tp_power):
    print("--------------------------------------------------------")
    print("depth\tper\tscore\ttp\t\tcm\ttp_power=%f" % tp_power)
    tp_filtered = 0
    fp_filtered = 0
    for d in range(depth_limit):
        tp_stat = {(percentage, score): val
                   for (depth, percentage, score), val in stat["tp"].items()
                   if d == depth}
        fp_stat = {(percentage, score): val
                   for (depth, percentage, score), val in stat["fp"].items()
                   if d == depth}
        tp = sum(tp_stat.values())

        filter, tp_f, fp_f = compute_best_filter_for_depth(
            tp_stat, fp_stat, tp, d, tp_power)
        tp_filtered += tp_f
        fp_filtered += fp_f
        print("%d\t(%d,\t%d)\t(%.2f%%,\t%.2f%%)" %
              (d, filter[0], filter[1], per(tp_f, tp_f + fp_f), per(tp_f, tp)))
    print("--------------------------------------------------------")
    return tp_filtered, fp_filtered


###############################################################################
# EXTRACT INFO FROM UPVC VCF ##################################################
###############################################################################




###############################################################################
# STATISTICS ON UPVC TP AND FP / (DEPTH, PERCENTAGE, SCORE) ###################
###############################################################################


def update_stat_no_check(stat, info):
    percentage_limit = 100
    score_limit = 41
    depth, percentage, score = info
    key = (depth if depth < depth_limit else depth_limit - 1,
           percentage if percentage < percentage_limit else percentage_limit - 1,
           score if score < score_limit else score_limit - 1)
    stat[key] = stat.setdefault(key, 0) + 1


def update_stat(stat, info):
    if stat is None:
        return
    update_stat_no_check(stat, info)


def update_stat_for_pos(stat, infos):
    if stat is None:
        return
    for info in infos.values():
        update_stat_no_check(stat, info)


def print_stat(tp_stats, fp_stats, nb_tp, nb_fp, nb_cm):
    print_limit = 100
    if tp_stats is None or fp_stats is None:
        return

    tp_acc_depth = 0
    tp_acc_percentage = 0
    tp_acc_score = nb_tp
    fp_acc_depth = 0
    fp_acc_percentage = 0
    fp_acc_score = nb_fp
    tp_stat = [[0, 0, 0] for _ in range(print_limit)]
    fp_stat = [[0, 0, 0] for _ in range(print_limit)]

    for (depth, percentage, score), val in tp_stats.items():
        tp_stat[depth][0] += val
        tp_stat[percentage][1] += val
        tp_stat[score][2] += val

    for (depth, percentage, score), val in fp_stats.items():
        fp_stat[depth][0] += val
        fp_stat[percentage][1] += val
        fp_stat[score][2] += val

    if nb_fp == 0:
        nb_fp = -1
    if nb_tp == 0:
        nb_tp = -1
    if nb_cm == 0:
        nb_cm = -1

    print("1%% tp = %.2f%% cm, 1%% cm = %.2f%% fp" %
          ((nb_tp / float(nb_cm)), (nb_cm / float(nb_fp))))
    print("i\t| tp/depth\t\tfp/depth\t| tp/percentage\t\tfp/percentage\t| tp/score\t\tfp/score")
    print("--------"
          "+---------------------------------------"
          "+---------------------------------------"
          "+---------------------------------------"
          )
    for i in range(print_limit):
        tp_depth = tp_stat[i][0]
        tp_per = tp_stat[i][1]
        tp_score = tp_stat[i][2]
        fp_depth = fp_stat[i][0]
        fp_per = fp_stat[i][1]
        fp_score = fp_stat[i][2]
        tp_acc_depth += tp_depth
        tp_acc_percentage += tp_per
        tp_acc_score -= tp_score
        fp_acc_depth += fp_depth
        fp_acc_percentage += fp_per
        fp_acc_score -= fp_score
        print("%d\t| %.2f%% (%.2f%%)   \t%.2f%% (%.2f%%)\t| %.2f%% (%.2f%%)   \t%.2f%% (%.2f%%)\t| %.2f%% (%.2f%%)   \t%.2f%% (%.2f%%)" %
              (i,
               per(tp_acc_depth, nb_tp), per(tp_depth, nb_tp),
               per(fp_acc_depth, nb_fp), per(fp_depth, nb_fp),
               per(tp_acc_percentage, nb_tp), per(tp_per, nb_tp),
               per(fp_acc_percentage, nb_fp), per(fp_per, nb_fp),
               per(tp_acc_score, nb_tp), per(tp_score, nb_tp),
               per(fp_acc_score, nb_fp), per(fp_score, nb_fp),
               )
              )


###############################################################################
# COMPUTE VCF #################################################################
###############################################################################


def compute_for_pos(v1_infos, v2_infos, tp, fp, tp_stat, fp_stat):
    for (ref, alt), v1_info in v1_infos.items():
        if (ref, alt) in v2_infos:
            tp += 1
            update_stat(tp_stat, v1_info)
        else:
            fp += 1
            update_stat(fp_stat, v1_info)
    return tp, fp


def compute(V1, V2, tp_stat, fp_stat):
    tp = 0
    fp = 0
    for (chr, pos), v1_infos in V1.items():
        if (chr, pos) in V2:
            tp, fp = compute_for_pos(
                v1_infos, V2[(chr, pos)], tp, fp, tp_stat, fp_stat)
        else:
            fp += len(v1_infos)
            update_stat_for_pos(fp_stat, v1_infos)
    return tp, fp


def print_VCF_quality(tp, fp, fn, cm, len_upvc, len_ref):
    print("tp:\t%.2f%%\t(%d/%d)" % (per(tp, len_upvc), tp, len_upvc))
    print("fp:\t%.2f%%\t(%d/%d)" % (per(fp, len_upvc), fp, len_upvc))
    print("fn:\t%.2f%%\t(%d/%d)" % (per(fn, len_ref), fn, len_ref))
    print("cm:\t%.2f%%\t(%d/%d)" % (per(cm, len_ref), cm, len_ref))


def compute_data(V_ref, V_upvc, len_ref, len_upvc):
    stats = {"tp": {}, "fp": {}} if args.enable_stat else None
    tp_stat = stats["tp"] if args.enable_stat else None
    fp_stat = stats["fp"] if args.enable_stat else None

    tp, fp = compute(V_upvc, V_ref, tp_stat, fp_stat)
    cm, fn = compute(V_ref, V_upvc, None, None)

    # print_stat(tp_stat, fp_stat, tp, fp, cm)

    if tp != cm:
        print("ERROR while computing TP et CM")
    if tp + fp != len_upvc:
        print("ERROR while computing TP and FP")
    if cm + fn != len_ref:
        print("ERROR while computing CM and FN")

    tp_power_tab = [0.5, 0.8, 1, 1.2, 1.5, 1.8, 2, 2.5, 3, 3.5, 4, 5, 6, 7, 8]
    if args.enable_stat:
        for tp_power in tp_power_tab:
            tp, fp = compute_best_filter(stats, tp_power)
            len_upvc = tp + fp
            new_fn = cm + fn - tp
            new_cm = tp

            if new_cm + new_fn != len_ref:
                print("ERROR while computing CM and FN")
            print_VCF_quality(tp, fp, new_fn, new_cm, len_upvc, len_ref)
    else:
        print_VCF_quality(tp, fp, fn, cm, len_upvc, len_ref)


chr_str_to_val = {"1": 1,
                  "2": 2,
                  "3": 3,
                  "4": 4,
                  "5": 5,
                  "6": 6,
                  "7": 7,
                  "8": 8,
                  "9": 9,
                  "10": 10,
                  "11": 11,
                  "12": 12,
                  "13": 13,
                  "14": 14,
                  "15": 15,
                  "16": 16,
                  "17": 17,
                  "18": 18,
                  "19": 19,
                  "20": 20,
                  "21": 21,
                  "22": 22,
                  "23": 23,
                  "24": 24,
                  "chr1": 1,
                  "chr2": 2,
                  "chr3": 3,
                  "chr4": 4,
                  "chr5": 5,
                  "chr6": 6,
                  "chr7": 7,
                  "chr8": 8,
                  "chr9": 9,
                  "chr10": 10,
                  "chr11": 11,
                  "chr12": 12,
                  "chr13": 13,
                  "chr14": 14,
                  "chr15": 15,
                  "chr16": 16,
                  "chr17": 17,
                  "chr18": 18,
                  "chr19": 19,
                  "chr20": 20,
                  "chr21": 21,
                  "chr22": 22,
                  "chr23": 23,
                  "chr24": 24,
                  "X": 23,
                  "Y": 24,
}


extract_info_re = re.compile("DEPTH=(.*);COV=(.*);SCORE=(.*)")


def extract_info(info):
    regex = re.search(extract_info_re, info)
    depth = int(regex.group(1))
    cov = int(regex.group(2))
    score = int(regex.group(3))
    percentage = 100.0
    if cov != 0:
        percentage = int(per(depth, cov))
    return (depth, percentage, score)


def process_line(line, SUB, INS, DEL, len_sub, len_ins, len_del, extract):
    if len(line) < 1 or line[0] == "#":
        return len_sub, len_ins, len_del

    chr_str, pos_str, _, ref, alt, _, _, info = line.split("\t")

    chr = chr_str_to_val[chr_str]
    pos = int(pos_str)

    ref_len = len(ref)
    alt_len = len(alt)
    if args.enable_stat and extract:
        info = extract_info(info)
    else:
        info = 0
    if ref_len == 1 and alt_len == 1:
        SUB.setdefault((chr, pos), {})[(ref, alt)] = info
        len_sub += 1
    elif ref_len > 1 and alt_len <= 1:
        DEL.setdefault((chr, pos), {})[(ref, alt)] = info
        len_del += 1
    elif alt_len > 1 and ref_len <= 1:
        INS.setdefault((chr, pos), {})[(ref, alt)] = info
        len_ins += 1
    return len_sub, len_ins, len_del


def get_data(filename, extract):
    print("\nreading " + filename)

    SUB = {}
    INS = {}
    DEL = {}
    len_sub = 0
    len_ins = 0
    len_del = 0

    if import_tqdm:
        with tqdm.tqdm(range(os.path.getsize(filename)), ascii = True) as progress_bar:
            for line in open(filename, "r").readlines():
                progress_bar.update(len(line))
                len_sub, len_ins, len_del = process_line(line, SUB, INS, DEL, len_sub, len_ins, len_del, extract)
    else:
        for line in open(filename, "r").readlines():
            len_sub, len_ins, len_del = process_line(line, SUB, INS, DEL, len_sub, len_ins, len_del, extract)

    print(len_sub, len_ins, len_del)
    return SUB, INS, DEL, len_sub, len_ins, len_del


SUB_ref, INS_ref, DEL_ref, len_ref_sub, len_ref_ins, len_ref_del = get_data(
    args.ref_file, False)
SUB_upvc, INS_upvc, DEL_upvc, len_upvc_sub, len_upvc_ins, len_upvc_del = get_data(
    args.upvc_file, True)

print("\nsubstitution")
compute_data(SUB_ref, SUB_upvc, len_ref_sub, len_upvc_sub)

print("\ninsertions")
compute_data(INS_ref, INS_upvc, len_ref_ins, len_upvc_ins)

print("\ndeletion")
compute_data(DEL_ref, DEL_upvc, len_ref_del, len_upvc_del)
