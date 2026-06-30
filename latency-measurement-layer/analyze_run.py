#!/usr/bin/env python3

import sys
import os
import argparse
import collections
import struct
import csv
import math
import statistics

DataPoint = collections.namedtuple('DataPoint', 'id stimulus submit queue_done present_done')

def read_csv(path):
    data_points = []
    with open(path, 'r') as csvfile:
        reader = csv.reader(csvfile)
        for row in reader:
            if row[0] == 'id':
                continue
            data_points.append(DataPoint(int(row[0]), float(row[1]), float(row[2]), float(row[3]), float(row[4])))

    return data_points

def analyze_frame_rate(data_points):
    total_time = data_points[-1].present_done - data_points[0].present_done
    frame_delta = data_points[-1].id - data_points[0].id
    print(f'Average frame time: {1000.0 * total_time / frame_delta:.5} ms ({frame_delta / total_time:.5} Hz)')

def avg_stddev_min_max_med(values):
    lo = 1e30
    hi = 0.0

    for value in values:
        lo = min(lo, value)
        hi = max(hi, value)

    return (statistics.mean(values), statistics.stdev(values), lo, hi, statistics.median(values))

def print_gap(text, comment, data):
    avg, stddev, lo, hi, med = data
    avg *= 1000.0
    stddev *= 1000.0
    lo *= 1000.0
    hi *= 1000.0
    med *= 1000.0

    print('')
    print(text + ':')
    print('  ' + comment)
    print(f'    Average {avg:.3} ms')
    print(f'      Standard Deviation +/- {stddev:.5} ms')
    print(f'    Median {med:.3} ms')
    print(f'    Range [{lo:.3}, {hi:.3}] ms')

def analyze_present_gaps(data_points):
    total_gaps = []
    for data in data_points:
        total_gaps.append(data.present_done - data.queue_done)
    print_gap('Gap between GPU idle to PresentComplete',
              'On VRR displays, this is ideally close to 0. If this is large, there is FIFO buffering',
              avg_stddev_min_max_med(total_gaps))

def analyze_submission_latency(data_points):
    total_gaps = []
    for data in data_points:
        total_gaps.append(data.queue_done - data.submit)

    print_gap('Gap between QueuePresent and GPU idle',
              'If this is large, we are likely GPU bound and would benefit from anti-lag',
              avg_stddev_min_max_med(total_gaps))

def analyze_stimulus_latency(data_points):
    total_gaps = []
    for data in data_points:
        total_gaps.append(data.submit - data.stimulus)
    print_gap('Gap between input stimulus and QueuePresent',
              'If this is large, we are likely CPU bound or application is buffering input a lot',
              avg_stddev_min_max_med(total_gaps))

def analyze_full_system_latency(data_points):
    total_gaps = []
    for data in data_points:
        total_gaps.append(data.present_done - data.stimulus)
    print_gap('Gap between input stimulus and PresentComplete',
              'Represents overall felt latency',
              avg_stddev_min_max_med(total_gaps))

def analyze_input_gpu_latency(data_points):
    total_gaps = []
    for data in data_points:
        total_gaps.append(data.queue_done - data.stimulus)
    print_gap('Gap between input stimulus and GPU idle',
              'Represents overall felt latency under ideal VRR conditions',
              avg_stddev_min_max_med(total_gaps))

def main():
    parser = argparse.ArgumentParser(description = 'Script for parsing profiling data.')
    parser.add_argument('--csv', type = str, help = 'The CSV.')

    args = parser.parse_args()
    if not args.csv:
        raise AssertionError('Need --csv.')

    data_points = read_csv(args.csv)
    analyze_frame_rate(data_points)

    analyze_full_system_latency(data_points)
    analyze_input_gpu_latency(data_points)
    analyze_stimulus_latency(data_points)
    analyze_submission_latency(data_points)
    analyze_present_gaps(data_points)

if __name__ == '__main__':
    main()