#!/usr/bin/env python3
"""M100A: count printf conversions vs. arguments for every vk_boot_logf* call.

Rationale: M99A crashed in strnlen because an added log line passed one argument
fewer than its format string demanded.  This checker is deliberately
conservative: it only understands string literals, nested parens/brackets,
char literals and comments outside literals.  If it cannot parse a call it says
so instead of guessing.
"""
import re
import sys

CONV = set("diouxXeEfgGaAcspn%")


def strip_comments(text, start, end):
    """Remove // and /* */ comments inside text[start:end], keeping literals."""
    out = []
    i = start
    while i < end:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < end:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == q:
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        if c == '/' and i + 1 < end and text[i + 1] == '/':
            j = text.find('\n', i)
            if j < 0 or j > end:
                j = end
            i = j
            continue
        if c == '/' and i + 1 < end and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = end if j < 0 else j + 2
            i = j
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def find_call_end(text, open_idx):
    """open_idx points at '('; return index of the matching ')' or -1."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_args(body):
    parts = []
    depth = 0
    cur = []
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n:
                if body[j] == '\\':
                    j += 2
                    continue
                if body[j] == q:
                    j += 1
                    break
                j += 1
            cur.append(body[i:j])
            i = j
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    parts.append(''.join(cur).strip())
    return parts


def count_conv(fmt):
    n = 0
    i = 0
    bad = []
    while i < len(fmt):
        if fmt[i] != '%':
            i += 1
            continue
        if i + 1 < len(fmt) and fmt[i + 1] == '%':
            i += 2
            continue
        j = i + 1
        while j < len(fmt) and fmt[j] in '#0-+ .0123456789':
            j += 1
        while j < len(fmt) and fmt[j] in 'lhqjzt':
            j += 1
        if j >= len(fmt) or fmt[j] not in CONV:
            bad.append(fmt[i:i + 6])
            i += 1
            continue
        n += 1
        i = j + 1
    return n, bad


def main(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    results = []
    for m in re.finditer(r'vk_boot_logf(_try)?\s*\(', text):
        open_idx = text.index('(', m.start())
        end = find_call_end(text, open_idx)
        if end < 0:
            results.append((text.count('\n', 0, m.start()) + 1, 'UNPARSED', 0, 0, []))
            continue
        body = text[open_idx + 1:end]
        body = strip_comments(text, open_idx + 1, end)
        parts = split_args(body)
        line = text.count('\n', 0, m.start()) + 1
        if not parts or not parts[0].startswith('"'):
            results.append((line, 'NOFMT', len(parts), 0, []))
            continue
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', parts[0])
        fmt = ''.join(lits)
        nconv, bad = count_conv(fmt)
        nargs = len(parts) - 1
        results.append((line, 'OK' if nconv == nargs and not bad else 'MISMATCH', nconv, nargs, bad))
    bad = [r for r in results if r[1] != 'OK']
    print(f"{path}: calls={len(results)} ok={len(results)-len(bad)} bad={len(bad)}")
    for line, kind, nconv, nargs, badc in bad:
        print(f"  line {line}: {kind} conv={nconv} args={nargs} badspec={badc}")
    return 1 if bad else 0


if __name__ == '__main__':
    rc = 0
    for p in sys.argv[1:]:
        rc |= main(p)
    sys.exit(rc)
