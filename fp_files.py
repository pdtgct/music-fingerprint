'''
    fp_files.py
    test functions
    
    Created by Peter Tanski on 27 June 2010.
    Copyright 2010 Zatisfi, LLC. MIT License, 2025
'''

from __future__ import with_statement

from collections import defaultdict
import contextlib
import cPickle
import csv
import difflib
from itertools import groupby
import math
import multiprocessing
from operator import itemgetter
import os
import Queue
import re
import sqlite3
import struct
import sys

import pymongo
import pymongo.objectid as objectid
import numpy
import psycopg2
import psycopg2.extensions as pg_exts

import utils.filesys as filesys
from attrdict import attrdict
try:
    import musicfp as mfp
except:
    print 'ERROR: unable to import musicfp'

try:
    import musiclib.matcher as matcher
    format_console_table = matcher.format_console_table
    import musiclib.musicdb as mdb
except ImportError:
    def format_console_table(headers, fields, formats, rows):
        '''Return a console table much like a table shown by MySQL Client over
        ``sequence`` of ``dicts``, ``rows``

        ``headers``
            ``sequence`` of ``string`` header names

        ``fields``
            ``sequence`` of ``string`` key names (if ``rows`` are ``dicts``),
            ``int`` indices (if ``rows`` are ``sequences``) or callables
            (if ``rows`` are objects)

        ``formats``
            ``sequence`` of ``pairs`` of::

              ( formatter, "ljust" | "rjust" )

            where ``formatter`` may be a ``string`` format, ``callable`` or
            ``None``; if ``None`` the output should be a ``string``

        ``rows``
            ``sequence`` of ``objects``; generally ``lists``, ``tuples`` or
            ``dicts`` but may be user-defined ``classes``

        .. NOTE:: this function does not take any account of current screen width

        '''

        maxs = map(len, headers)

        tbl_rows = []
        for r in rows:
            vals = []
            for fn in fields:
                vals.append(r[fn])
            tbl_row = []
            for val, (cvtr, just) in zip(vals, formats):
                if not cvtr:
                    if val is None:
                        val = ''
                    tbl_row.append(val)
                elif callable(cvtr):
                    try:
                        tbl_row.append(cvtr(val))
                    except:
                        tbl_row.append(str(val))
                else:
                    try:
                        tbl_row.append(cvtr % val)
                    except:
                        tbl_row.append(str(val))
            maxs = list(max(m, len(v)) for m, v in zip(maxs, tbl_row))
            tbl_rows.append(tbl_row)
        header_fmts = list(' %%-%ds ' % mx for mx in maxs)
        row_fmts = []
        lines = []
        for mx_len, (cvtr, just) in zip(maxs, formats):
            if just == 'ljust':
                row_fmts.append(' %%-%ds ' % mx_len)
            else:
                row_fmts.append(' %%%ds ' % mx_len)
            lines.append('-' * (mx_len + 2))
        header_str = '|%s|' % '|'.join(h.center(mx + 2)
                                       for h, mx in zip(headers, maxs))
        row_fmt = '|%s|' % '|'.join(row_fmts)
        line_fmt = '+%s+' % '+'.join(lines)

        tbl_strs = [line_fmt, header_str, line_fmt]
        for r in tbl_rows:
            tbl_strs.append(row_fmt % tuple(r))
        tbl_strs.append(line_fmt)
        return '\n'.join(tbl_strs)

@contextlib.contextmanager
def cmongo_conn(mconn=None, host='localhost', port=27017):
    close_conn = False
    try:
        if not mconn:
            mconn = pymongo.Connection(host=host, port=port)
            close_conn = True
        yield mconn
    except:
        raise
    finally:
        if close_conn:
            mconn = mconn and mconn.disconnect()

def mongo_conn(host='localhost', port=27017):
    return pymongo.Connection(host, port)

def cross_matches(dist_lst, start=0, end=0):
    cds = []
    for i, (fn1, fp1) in enumerate(dist_lst):

            c = mfp.match_chromap(fp1, fp2)#, start=start, end=end)
            m = mfp.match_fingerprints(fp1, fp2)
            cds.append((fn1, fn2, c, m))
    return cds

############################################################
# SQLite Stuff
############################################################

def s3blob_to_fprint(fp_blob_buf):
    '''SQlite3 converter to convert a BLOB to a musicfp.Fingerprint
    '''
    return cPickle.loads(fp_blob_buf[:])

def sqlite_row_fp(rw):
    '''From a sqlite3 row of the form::

     [fpath, s_oid, title, artist, album, bitrate, channels, format,
      fingerprint]

    return a ``pair`` of::

      (attrdict(fpath, oid, title, artist, album, bitrate, channels, format),
       musicfp.Fingerprint)

    '''
    fd = attrdict(
        fpath=rw[0],
        oid=rw[1],
        title=rw[2],
        artist=rw[3],
        album=rw[4],
        bitrate=rw[5],
        channels=rw[6],
        format=rw[7])
    fp = s3blob_to_fprint(rw[8])
    # no need to resize fp.cprint
    # we want to store the full-size fingerprint
    # GiST will take parts necessary for the keys
    return (fd, fp)

CREATE_RESULTS_TABLE = """\
create table if not exists results (
  cp    REAL NOT NULL,
  fm    REAL NOT NULL,
  sm    REAL NOT NULL,
  nm    TEXT NOT NULL,
  oid1  TEXT NOT NULL,
  oid2  TEXT NOT NULL
)"""

def insert_sqlite_results(sconn, results):
    if not results:
        return
    sql = 'insert into results values(?, ?, ?, ?, ?, ?)'
    try:
        if len(results) == 1:
            c = sconn.execute(sql, results[0])
        else:
            c = sconn.executemany(sql, results)
    except:
        pass
    else:
        sconn.commit()

############################################################
# Postgress Stuff
############################################################

def pg_cast_fprint(val, pg_crsr):
    '''Function executed by psycopg2 to create a Fingerprint
    from a field in a returned row with a `fprint` type.

    This function must be registered using
    `register_pg_fprint_type`, in thie module.

    '''
    return mfp.from_string(val)

def pg_adapt_fprint(fprint):
    '''Function executed by psycopg2 when inserting a fingerprint
    into an insert query.  This is not only convenient: it allows
    ``execute_many`` on that type without having to manually format
    each row.
    '''
    return pg_exts.AsIs(repr(str(fprint)))

def pg_register_fprint(pg_crsr):
    '''Register insert query ("adapt") and select result ("cast")
    conversion for ``musicfp.Fingerprint`` types in ``psycopg2``

    ``pg_crsr``
        ``psycopg2._psycopg.cursor`` instance open to a database
        with the ``fprint`` type available

    .. NOTE:: See http://initd.org/psycopg/docs/advanced.html#adapting-new-python-types-to-sql-syntax
              and http://www.python.org/peps/pep-0246.html

    '''
    pg_exts.register_adapter(mfp.Fingerprint, pg_adapt_fprint)
    pg_crsr.execute('select NULL::fprint')
    oid = pg_crsr.description[0][1]
    fprint_typ = pg_exts.new_type((oid,), 'fprint', pg_cast_fprint)
    pg_exts.register_type(fprint_typ)

SQL_INS_PG_ROW = (
    'insert into fingerprints (soid, fingerprint) values(%s, %s::fprint)')
def get_row(r):
    fd, fp = r
    return (fd.oid, fp)

def insert_pg_rows(pconn, pcrsr, rows, sql_ins=SQL_INS_PG_ROW):
    ins_rows = []
    for i, r in enumerate(rows):
        ins_rows.append(get_row(r))
        if i % 1000 == 0:
            pcrsr.executemany(sql_ins, ins_rows)
            pconn.commit()
            ins_rows = []

SQL_FIND_FPRINT_MATCH = """\
select f1.soid, fprint_cmp(%s::fprint, f1.fingerprint) as fcmp
from fingerprints as f1
where %s::fprint ~= f1.fingerprint
order by fcmp desc"""
SQL_FIND_OID_MATCH = """\
select f2.soid, fprint_cmp(f1.fingerprint, f2.fingerprint) as fcmp
from fingerprints as f1, fingerprints as f2
where f1.soid=%s
and f1.soid <> f2.soid
and f1.fingerprint ~= f2.fingerprint
order by fcmp desc"""
SQL_FIND_OID_EXTMATCHES = """\
select f2.soid, fprint_cmp(f1.fingerprint, f2.fingerprint) as fcmp
from fingerprints as f1, fingerprints as f2
where f1.soid=%s
and f1.soid <> f2.soid
and f2.soid <> %s
and f1.fingerprint ~= f2.fingerprint
order by fcmp desc"""
def find_matching(pc, pr, oid=None):
    if not (oid or fprint):
        raise ValueError('one of soid or fprint is requird')
    rows = None
    if oid:
        pr.execute(SQL_FIND_OID_MATCH, (str(oid),))
        rows = pr.fetchall()
        
    if not rows:
        return []
    
    oids_st = set(o for o, v in rows)
    skip_oids = set()
    for oid2, mval in rows:
        pr.execute(SQL_FIND_OID_EXTMATCHES, (oid2, oid))
        extrows = pr.fetchall()
        if extrows:
            mval_min = mval + (mval * 0.05)
            for o, v in extrows:
                if v > mval_min:
                    print(
                        'skipping overreach: aval %.16f, mval_min %.16f'
                        % (v, mval_min))
                    skip_oids.add(o)

    return filter(lambda (od,vn): od not in skip_oids, rows)

############################################################
# Raw Matching (in Python)
############################################################

def detect_bad(fd_fp):
    '''Return an error message telling how the Fingerprint
    music file is bad, or None if it is not bad.

    ``fd_fp``
        ``pair`` of file attrdict (with member ``["properties"]["length"]``
        and ``Fingerprint``.  The length property is used because FFMPEG
        has a bug that skips the ``VBR`` ``Xing`` header and reports an
        invalid song length (roughly song length * number of channels)
        but there is no way for us to tell when that is bad.  

    .. NOTE:: so far this just detects silent tracks and
              bad fingerprints
    
    '''
    fd, fp = fd_fp
    dom = fp.dom
    if (dom == 0).all():
        return 'slient track: dom is 0'
    min_songlen = fd['properties']['length']
    if not min_songlen:
        min_songlen = fp.songlen
    expect_size = int(math.floor(float(min(min_songlen, 60)) * 15.8))
    if fp.cprint.size < expect_size:
        return 'small chromaprint length: unable to fingerprint'
    return None

re_words = re.compile('([^\W]+|[^\s]+)', re.U)
def to_name(tg):
    artist = tg.artist
    title = tg.title
    m = ''.join(re_words.findall(artist) + re_words.findall(title)).lower()
    nm = '_'.join((artist, title))
    return nm, m

# this only works on linuxy machines because we fork
songs = []
n_songs = 0
cm_threshold = 0.9
fm_threshold = 0.8
fm_check_adder = 0.6
def matchup_songs(ix):
    global songs
    global n_songs
    results = []
    #with cmongo_conn() as pmc:
    #    test = pmc.test
    #if ix == n_songs - 1:
    #    return []
    tg1, fp1 = songs[ix]
    oid1 = str(tg1['oid'])
    br1 = fp1.bit_rate
    ln1 = float(fp1.songlen)
    nm1, m1 = to_name(tg1)
    sq = difflib.SequenceMatcher(None, a=m1)
    for tg2, fp2 in songs[ix+1:]:
        ln2 = float(fp2.songlen)
        # skip match if songlen difference > 10% of min size
        if abs(ln1 - ln2) / min(ln1, ln2) > .1:
            continue
        fm = mfp.match_fingerprints(fp1, fp2)
        cp = mfp.match_chromap(fp1, fp2)
        if not (fm > .72 and cp > .72):
            continue
        nm2, m2 = to_name(tg2)
        sq.set_seq2(m2)
        sm =  sq.ratio()
        nm = '__'.join((nm1, nm2))
        results.append((cp, fm, sm, nm, oid1, str(tg2['oid'])))
    return results

def run_matches(inp_songs, sconn):
    global songs
    global n_songs
    with cmongo_conn() as mc:
        #songs.extend(pull_fingerprints(qry={'title': {'$exists': True}}))
        songs.extend(inp_songs)
        n_songs = len(songs)
        ixs = xrange(n_songs)
        p = multiprocessing.Pool(2)
        matches = p.imap(matchup_songs, ixs)
        n_matches = 0
        for rows in matches:
            insert_sqlite_results(sconn, rows)
            n_matches += 1
        songs = []
        return n_matches

def p_work(fn):
    try:
        fp = mfp.fingerprint(fn)
        return insert_fingerprint((fn, fp))
    except:
        return None

def run_pool(fns):
    p = multiprocessing.Pool(2)
    return filter(None, p.map(p_work, fns))

re_base = re.compile(r'_\d{5}_s_\d{2,3}(?:_vbr)?_-50_\.(?:mp3|flac)$', re.I)
re_base_vl = re.compile(
    r'_(?P<freq>\d{5})_s_(?P<br>\d{2,3})(?:_(?P<vbr>vbr))?_-50_'
    r'\.(?:(?P<enc>mp3|flac))$',
    re.I)
def is_base_file(fpath):
    return not re_base.search(fpath)

def group_files(base_dir, update_tags=False):
    fpaths = filesys.find_files(base_dir, [r'(?i)\.(mp3|flac)$'])
    dfpaths = defaultdict(list)
    for k, g in groupby(fpaths, key=os.path.dirname):
        dfpaths[k].extend(g)
    for dn, fls in dfpaths.iteritems():
        dfls = {}
        if update_tags:
            dtags = {}
        fls.sort()
        ext_fnames = []
        for fn in fls:
            if is_base_file(fn):
                bn = os.path.basename(fn[:-4])
                dfls[bn] = []
                if update_tags:
                    dtags[bn] = matcher.music_file(fn)
            else:
                ext_fnames.append(fn)
        for efn in ext_fnames:
            bn = os.path.basename(re_base.sub('', efn))
            if update_tags:
                fd = dtags[bn]
                tags = fd.tags
                if not mdb.update_tags(efn, tags):
                    print('unable to update tags in ' + efn)
            dfls[bn].append(efn)
        dfpaths[dn] = dfls
    return dfpaths

def update_fprints(base_dir):
    fpaths = filesys.find_files(base_dir, [r'(?i)\.(mp3|flac)$'])
    mc = None
    try:
        mc = pymongo.Connection()
        tfprints = mc.test.fprints
        for fn in fpaths:
            fd = matcher.music_file(fn)
            props = fd.properties
            d = {
                'enc': os.path.splitext(fn)[1],
                'sample_rate': props.sample_rate,
                'bitrate': props.bitrate,
                'channels': props.channels,
                }
            tfprints.update({'fn':fn}, {'$set':d}, upsert=False, multi=False)
    finally:
        mc = mc and mc.disconnect()

def matchup_group(td_fps):
    rows = []
    for i, (t1, f1) in enumerate(td_fps):
        m1 = re_base_vl.search(t1['fn'])
        is_vbr = 0
        vol_change = 0
        if m1:
            is_vbr = m1.group('vbr') and 1 or 0
            vol_change = -50
        base_row = (
            t1['artist'],
            t1['title'],
            t1.get('enc', 'mp3'),
            t1.get('sample_rate', 44100),
            t1.get('bitrate', 320),
            t1.get('channels', 2),
            is_vbr,
            vol_change,
            )
        c1 = f1.cprint
        c1_sz = c1.size
        for t2, f2 in td_fps[i+1:]:
            c2 = f2.cprint
            mn_sz = min(c1_sz, c2.size)
            #cp = mfp.match_chromaf(f1, f2, start=240, end=368)
            cp = mfp.match_chromap(f1, f2)
            fm = mfp.match_fingerprints(f1, f2)
            m2 = re_base_vl.search(t2['fn'])
            is_vbr = 0
            vol_change = 0
            if m2:
                is_vbr = m2.group('vbr') and 1 or 0
                vol_change = -50
            row = base_row + (
                t2.get('enc', 'mp3'),
                t2.get('sample_rate', 44100),
                t2.get('bitrate', 320),
                t2.get('channels', 2),
                is_vbr,
                vol_change,
                cp, fm
                )
            rows.append(row)
    return rows

re_esc = re.compile(r'([\[\]\(\)\+\*\.\$\^\?])')
def compare_groups(grouped_fpaths):
    '''Return a table of::

     [ (artist, title, enc1, br1, fq1, cn1, is_vbr1, vol_change1
        enc2, br2, fq2, cn2, is_vbr2, vol_change2, cp, fm) ]

    The comparisons here should be fast so it is run serially.

    '''
    rheaders = (
        'artist', 'title',
        'enc1', 'br1', 'fq1', 'cn1', 'is_vbr1', 'vol_change1',
        'enc2', 'br2', 'fq2', 'cn2', 'is_vbr2', 'vol_change2',
        'cp', 'fm',
        )
    all_oids = []
    all_rows = []
    with cmongo_conn() as mc:
        test = mc.test
        reg_results = test.reg_results
        tfprints = test.fprints
        reg_results.ensure_index([('artist', pymongo.ASCENDING),
                                  ('title', pymongo.ASCENDING)])
        for gd, bg in grouped_fpaths.iteritems():
            for bn, fpths in bg.iteritems():
                rbn = re_esc.sub(r'\\\1', bn)
                fps = pull_fingerprints(
                    mconn=mc,
                    qry={'fn': re.compile('^' + gd + '/' + rbn)})
                rows = matchup_group(fps)
                if rows:
                    #oids = reg_results.insert(
                    #    [dict(zip(rheaders, r)) for r in rows])
                    #all_oids.extend(oids)
                    all_rows.extend(rows)
    return (all_oids, all_rows)

def compare_test_music(dpath):
    rows = []
    with cmongo_conn() as mc:
        test = mc.test
        tfprints = test.fprints
        fps = pull_fingerprints(
            mconn=mc,
            qry={'fn': re.compile('^' + dpath + '/')})
        rows = matchup_group(fps)
    return rows

def test_music_to_csv(csv_fpath, rows):
    rheaders = (
        'artist', 'title',
        'enc1', 'br1', 'fq1', 'cn1', 'is_vbr1', 'vol_change1',
        'enc2', 'br2', 'fq2', 'cn2', 'is_vbr2', 'vol_change2',
        'cp', 'fm',
        )
    with open(csv_fpath, 'w') as fo:
        wtr = csv.writer(fo, lineterminator='\n', quoting=csv.QUOTE_MINIMAL, delimiter='\t')
        wtr.writerow(rheaders)
        for r in rows:
            nr = []
            for v in r:
                if isinstance(v, unicode):
                    v = v.encode('utf-8')
                    vl = v.lower()
                    if vl == '.mp3':
                        v = 'mp3'
                    elif vl.startswith('.fla'):
                        v = 'flac'
                else:
                    v = str(v)
                nr.append(v)
            wtr.writerow(nr)

############################################################
# Postprocessing Results (some useful grouping)
############################################################

class SP(tuple):
    __slots__ = ()

    def __new__(self, un, ixs):
        return tuple.__new__(cls, (un, ixs))

    @property
    def un(self):
        return self[0]

    @property
    def ixs(self):
        return self[1]

class Match(tuple):
    __slots__ = ()

    def __new__(self, ix1, ix2, val):
        return tuple.__new__(cls, (ix1, ix2, val))

    @property
    def ix1(self):
        return self[0]

    @property
    def ix2(self):
        return self[1]

    @property
    def val(self):
        return self[2]

def calc_cpfm(cp, fm):
    return ((  0.012985
             +  .263439 * fm
             + -.683234 * cp
             + 1.592623 * (cp**3)
            + 0.06348) / 1.2489  )

def get_graph_songs(sc, table_name):
    '''Return a networkx.Graph of the results
    '''
    import networkx as nx
    g = nx.Graph()
    crsr = sc.execute('select oid1, oid2, cp, fm from %s' % table_name)
    g.add_weighted_edges_from(((oid1, oid2, calc_cpfm(cp, fm))
                               for oid1, oid2, cp, fm in crsr.fetchall()))
    return g

def find_suspect_songs(gtable, mtable, sm_scores):
    '''
    '''
    suspect_songs = []
    for oid, sms in sm_scores.iteritems():
        mn_sm = numpy.mean(sms)
        if mn_sm < .7 and len(sms) > 3:
            suspect_songs.append((oid, mn_sm, sms))
    return suspect_songs

re_album_parens = re.compile(
    r'([\[(]\s*.*?(bootleg|(?:bonus\s+)?cd[0-9]*|clean(?:\s+edit)?|'
     '(?:bonus\s+)?dis[ckq]u?e?\s*[a-z0-9-]*|ep(?:\s+single))|'
     'edit(?:ed|ion)?|live|(?:bonus\s+)?track?\s*(?:[a-z0-9-]+|edition)?|'
     '(?:cd|ep\s+)?single(:?\s+track?)?|'
     'v(?:(?:ol(?:\.|ume)?\s*[a-z0-9]*|ersi?o?n?)|vinyl)\s*[\])])',
    re.I | re.U)

re_disctrack = re.compile(r'((?:dis[ckq]u?e?|track?)\s*[a-z0-9-]+)',
                          re.I | re.U)

re_live = re.compile(r'(live\s+in|.+?\blive\s*$)', re.I | re.U)
def matchup_dupes(fcoll, gtable, mtable, update_db=True):
    '''Return a ``pair`` of::

      (suspect_songs, dupes_table)

    from de-duping the collection in ``gtable``

    .. WARNING:: the size of suspect_songs will likely be long since we
                 compared songs without considering album and here we also
                 consider album

    '''
    OBJID = objectid.ObjectId
    dupes_table = defaultdict(set)
    suspect_songs = []
    for oid1, st_oids in gtable.iteritems():
        if oid1 in dupes_table:
            continue

        fd1 = fcoll.find_one({'_id': OBJID(oid1)})
        fds = [fd1]

        ars1 = fd1['artists']
        st_ars1 = set(ars1)
        al1 = fd1['tags']['album'].lower()
        sq1 = difflib.SequenceMatcher(
            None,
            a=''.join(re_words.findall(al1)))
        ti1 = fd1['tags']['title'].lower()
        is_live_al = bool(re_live.search(al1))
        is_clean_al = False
        m_paren = re_album_parens.search(al1)
        paren_al1 = ''
        if m_paren and 'clean' in m_paren.group(1):
            is_clean_al = True
            paren_al1 = m_paren.group(2)
        is_live_ti = bool(re_live.search(ti1))
        is_clean_ti = False
        paren_ti1 = ''
        m_paren = re_album_parens.search(ti1)
        if m_paren and 'clean' in m_paren.group(1):
            is_clean_ti = True
            paren_ti1 = m_paren.group(2)

        for oid2 in st_oids:
            try:
                cp, fm, sm = mtable[(oid1, oid2)]
            except KeyError:
                cp, fm, sm = mtable[(oid2, oid1)]
            fd2 = fcoll.find_one({'_id': OBJID(oid2)})
            ars2 = fd2['artists']
            al2 = fd2['tags']['album'].lower()
            ti2 = fd2['tags']['title'].lower()
            if al1 != al2:
                sq1.set_seq2(''.join(re_words.findall(al2)))
                al_rt = sq1.ratio()
                if al_rt < .5:
                    sq1.set_seq2(''.join(re_words.findall(
                        re_album_parens.sub('', al2))))
                    al_rt = sq1.ratio()
                    if al_rt < .5:
                        continue

            if sm != 1.0:
                if is_live_al != bool(re_live.search(al2)):
                    continue

                is_clean_al2 = False
                m_paren2 = re_album_parens.search(al2)
                paren_al2 = ''
                if m_paren2 and 'clean' in m_paren2.group(1):
                    is_clean2 = True
                    paren_al2 = m_paren2.group(2)

                if is_live_ti != bool(re_live.search(ti2)):
                    continue

                is_clean_ti2 = False
                paren_ti2 = ''
                m_paren2 = re_album_parens.search(ti2)
                if m_paren2 and 'clean' in m_paren2.group(1):
                    is_clean_ti2 = True
                    paren_ti2 = m_paren2.group(2)

                if is_clean_al != is_clean_al2 or is_clean_ti != is_clean_ti2:
                    continue

                st_ars2 = set(ars2)
                if (st_ars1.isdisjoint(st_ars2) or
                    (sm < .8 or
                     (paren_al1 != paren_al2 or paren_ti1 != paren_ti2))):
                    continue
            fds.append(fd2)

        if len(fds) == 1:
            suspect_songs.append((oid1, fd1))
            continue

        bers = []
        for fd in fds:
            # -errors so reverse sort for bitrate sorts errors ascending
            bers.append(((fd['properties']['bitrate'], -fd['num_errors']), fd))
        bers.sort(key=itemgetter(0), reverse=True)

        n_bers = len(bers)
        (br, be), bfd = bers[0]
        ix = 1
        # should be rare but there are 530 songs where num_erros > 1
        while be < -1 and ix < n_bers:
            (br, be), bfd = bers[ix]
            ix += 1

        if be < -1:
            bers.sort(key=lambda ((r,e),f): (e, r), reverse=True)
            (br, be), bfd = bers[0]

        if update_db:
            bf_id = bfd['_id']
            best_of = []
            for t, fd2 in bers:
                fd2_id = fd2['_id']
                if fd2_id != bf_id:
                    fcoll.update(
                        {'_id': fd2_id}, {'$addToSet': {'dupe_of': bf_id}},
                        upsert=False, multi=False)
                    best_of.append(fd2_id)
                    dupes_table[str(fd2_id)].add(str(bf_id))
            fcoll.update({'_id': bf_id}, {'$set': {'best_of': best_of}},
                         upsert=False, multi=False)

    return suspect_songs, dupes_table

