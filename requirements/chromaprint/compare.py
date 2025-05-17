import re

import numpy as np
import gmpy

re_ar = re.compile(r'^ARTIST=(.*?)$')
re_ti = re.compile(r'^TITLE=(.*?)$')
re_al = re.compile(r'^ALBUM=(.*?)$')
re_len = re.compile(r'^LENGTH=(\d+)$')
re_bits = re.compile(r'^BITRATE=(\d+)$')
re_fmt = re.compile(r'^FORMAT=(.*?)$')
re_fp = re.compile(r'^FINGERPRINT=(.*?)\s*$')
def parse_file(fpath):
    fps = []
    with open(fpath, 'rb') as fi:
        ar, ti, al = None, None, None
        length, bits, fmt = None, None, None
        fp = None
        for ln in fi:
            if not ar:
                m = re_ar.match(ln)
                if m:
                    ar = m.group(1)
            elif not ti:
                m = re_ti.match(ln)
                if m:
                    ti = m.group(1)
            elif not al:
                m = re_al.match(ln)
                if m:
                    al = m.group(1)
            elif not length:
                m = re_len.match(ln)
                if m:
                    length = m.group(1)
            elif not bits:
                m = re_bits.match(ln)
                if m:
                    bits = m.group(1)
            elif not fmt:
                m = re_fmt.match(ln)
                if m:
                    fmt = m.group(1)
                if fmt != 'MP3':
                    ar, ti, al = None, None, None
                    length, bits, fmt = None, None, None
                    fp = None
            else:
                m = re_fp.match(ln)
                if m:
                    fp = m.group(1).split(' ')
                    fs = []
                    for n in fp:
                        if not n:
                            continue
                        n = int(n, 16)
                        fs.append(n)
                    arr = np.array(fs, dtype=np.uint32)
                    d = dict(
                        artist=ar,
                        title=ti,
                        album=al,
                        length=int(length),
                        bits=int(bits),
                        format=fmt,
                        fprint=arr
                        )
                    fps.append(d)
                    ar, ti, al = None, None, None
                    length, bits, fmt = None, None, None
                    fp = None
    return fps

np_popcount = np.frompyfunc(gmpy.popcount, 1, 1)

def match_wprints(wp1, wp2):
    w1 = wp1['fprint']
    w1s = w1.size
    w2 = wp2['fprint']
    w2s = w2.size
    ml = min(w1s, w2s)
    # could shift w2 forward 120 then backward 120 (min by length)
    # could choose "w1" as shorter of the two (w2 has longer to shift)
    sm = np_popcount(w1[:ml] ^ w2[:ml]).sum()
    if sm < 1:
        return 1.0
    ttot = float(ml) * 32.
    if ttot < 0.0:
        return 0.0
    return  1.0 - float(sm) / ttot

# constants
mxof = 120
mxbe = 2
def match_cprints(cp1, cp2):
    '''Much slower and not much better than simple
    max of hamming distance.

    for two songs that match wprint: 0.35461663066954646
    this returns a match cprint: 0.33693304535637147
    
    '''
    c1 = cp1['fprint']
    c2 = cp2['fprint']
    c1_sz = c1.size
    c2_sz = c2.size
    mx_sz = max(c1_sz, c2_sz)
    cnts = [0] * (mx_sz * 2 + 1)
    for i in xrange(c1_sz):
        jb = max(0, i  - mxof)
        je = min(c2_sz, i + mxof)
        vi = c1.item(i)
        for j in xrange(jb, je):
            vj = c2.item(j)
            be = gmpy.popcount(vi ^ vj)
            if be < mxbe:
                cnts[i - j + mx_sz] += 1
    tc = max(cnts)
    print 'tc: ', tc
    if tc == 0:
        return 0.0
    return 1.0 - float(tc) / float(min(c1_sz, c2_sz))

def cross_dists(d_fprints):
    cds = []
    for i, d1 in enumerate(d_fprints):
        for d2 in d_fprints[i+1:]:
            c = match_wprints(d1, d2)
            cds.append((d1, d2, c))
    return cds

def cross_dcp(d_fprints):
    cds = []
    i = 0
    n_fps = len(d_fprints)
    while i < n_fps:
        d1 = d_fprints[i]
        for d2 in d_fprints[i+1:]:
            c = match_cprints(d1, d2)
            cds.append((d1, d2, c))
        i += 1
    return cds

