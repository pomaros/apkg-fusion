#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Check exported native ARexx verification reports against independent CLI output.

The retained job must be read again after another verification replaces the
window's report. Inputs are independently exported test outputs, not screenshots.
"""
import argparse
from pathlib import Path
import re


def cli_report(text):
    report, separator, tail = text.partition('\nstatus=')
    if not separator or not re.fullmatch(r'0 outcome=0\nresult(?:=| )0\n?', tail):
        raise ValueError('CLI verification did not complete successfully')
    counts = re.findall(r'^(ok|changed|missing|unknown) ([0-9]+)$', report, re.M)
    if len(counts) != 4 or {name for name, _ in counts} != {'ok', 'changed', 'missing', 'unknown'}:
        raise ValueError('missing or duplicate verification counts')
    return report, dict(counts)


def job_report(text):
    match = re.fullmatch(r'command=JOB(?: [0-9]+)? FIELD message\nrexx_rc=0\n(.*)\nresult(?:=| )0\n?', text, re.S)
    if not match or not match[1].strip():
        raise ValueError('ARexx job has no successful verification report')
    return match[1]


def expected_message(report):
    # rx_job owns 480 bytes; ARexx renders control characters as spaces.
    prefix = report.encode()[:479].decode()
    return ''.join(' ' if ord(c) < 32 or ord(c) == 127 else c for c in prefix)


def verify(first_cli, first_job, second_cli, second_job, retained_job):
    first, counts = cli_report(first_cli)
    second, next_counts = cli_report(second_cli)
    if counts == next_counts:
        raise ValueError('the second verification must exercise different results')
    if job_report(first_job) != expected_message(first):
        raise ValueError('first job differs from independent CLI verification')
    if job_report(second_job) != expected_message(second):
        raise ValueError('second job differs from independent CLI verification')
    if job_report(retained_job) != expected_message(first):
        raise ValueError('completed job report changed after worker/window replacement')
    return 'PASS: native verification counts, bounded job report and retained ownership'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('first-cli', 'first-job', 'second-cli', 'second-job', 'retained-job'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        print(verify(*(getattr(args, key).read_text() for key in
                       ('first_cli', 'first_job', 'second_cli', 'second_job', 'retained_job'))))
    except ValueError as error:
        parser.exit(1, str(error) + '\n')
