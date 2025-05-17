#!/usr/bin/env python
'''
    reader.py
    bulk read fingerprints into a database at HOST and PORT

    *Requirements*
      - pymongo
      - numpy
      - musicfp (fingerprint library)
      - filesys (personal module)

    Created by Peter Tanski on 27 June 2010.
    Copyright 2010 Zatisfi, LLC. MIT License, 2025

'''

from __future__ import with_statement

import cPickle
import errno
import gc
import multiprocessing
import optparse
import os
import Queue
import re
import sqlite3
import sys

import pymongo
import pymongo.objectid as objectid
import numpy as np

import filesys
import musicfp as mfp


__docformat__ = 'reStructuredText'


DEFAULT_MONGO_PORT = 27017
DEFAULT_MONGO_DN = 'music'
DEFAULT_MONGO_CN = 'files'
DEFAULT_MONGO_ARGS = (
    'lavender',
    DEFAULT_MONGO_PORT,
    DEFAULT_MONGO_DN,
    DEFAULT_MONGO_CN)
FINGERPRINTS_TABLE_SQL = """\
create table if not exists fingerprints (
 fpath       TEXT UNIQUE,
 oid         TEXT UNIQUE,
 title       TEXT,
 artist      TEXT,
 album       TEXT,
 bitrate     INTEGER,
 channels    INTEGER,
 format      TEXT,
 fingerprint BLOB
)"""
INCREMENT = 1000
CHUNK_SIZE = 10

def fprint_to_s3blob(fp):
    '''SQlite3 converter to convert a musicfp.Fingerprint to a BLOB
    '''
    return buffer(cPickle.dumps(fp, protocol=cPickle.HIGHEST_PROTOCOL))

def s3blob_to_fprint(fp_blob_buf):
    '''SQlite3 converter to convert a BLOB to a musicfp.Fingerprint
    '''
    return cPickle.loads(fp_blob_buf[:])

def insert_fingerprints(sconn, fps):
    '''Insert all the fingerprints in the ``list`` of ``tuples`` ``fps``
    into the ``"fingerprints"`` table.

    ``fps``
        ``pair`` of ``(music_file_dict, musicfp.Fingerprint)``

    '''
    if not fps:
        return
    rows = []
    for fd, fp in fps:
        tags = fd['tags']
        props = fd['properties']
        if any(nm not in tags for nm in ('title', 'artist', 'album')):
            print('INVALID FILE TAGS for "%r"' % fd['fpath'].encode('utf-8'))
            continue
        rows.append((
            fd['fpath'],
            str(fd['_id']),
            tags['title'],
            tags['artist'],
            tags['album'],
            props['bitrate'],
            props['channels'],
            os.path.splitext(fd['fpath'])[1][1:],
            fprint_to_s3blob(fp),
            ))
    n_rows = len(rows)
    if n_rows == 0:
        return n_rows
    vs = ','.join(['?'] * len(rows[0]))
    sql = 'insert into fingerprints values(%s)' % vs
    try:
        if n_rows == 1:
            nc = sconn.execute(sql, rows[0])
        else:
            nc = sconn.executemany(sql, rows)
        sconn.commit()
    except sqlite3.IntegrityError, err:
        print('ERROR: inserting %d rows: %s' % (n_rows, err))
    return n_rows

def read_fprints(wid, pconn, que, base_fpath):
    '''Read fingerprints from file dicts in the queue
    '''
    ret_files = []
    while 1:
        try:
            fds = que.get(timeout=30.0)
        except Queue.Empty:
            break
        if not fds:
            continue
        for fd in fds:
            mfpath = base_fpath + fd['fpath'].encode('utf-8')
            try:
                if not os.path.exists(mfpath):
                    print('MISSING: file "%s"' % mfpath)
                    continue
                else:
                    fp = mfp.fingerprint(mfpath)
            except Exception, e:
                print('ERROR: reading music file "%s"' % mfpath)
            else:
                print('read: "%s"' % mfpath)
                ret_files.append((fd, fp))
        if ret_files:
            pconn.send(ret_files)
            # force a wait on this
            try:
                ok = pconn.recv()
            except EOFError:
                continue
            else:
                del ret_files[:]
                gc.collect()

    print('Worker %d complete' % wid)
    pconn.close()
    sys.exit(0)

def get_next_chunks(files_coll, qry, start, limit):
    '''Return file dicts chunked into lists of 10
    '''

    if isinstance(start, int) and isinstance(limit, int):
        fds = files_coll.find(qry, skip=start, limit=limit)
    else:
        fds = files_coll.find(qry)
    chunks = []
    chunk = []    
    re_mf = re.compile(r'\.(mp3|flac)$', re.U | re.I)
    for i, fd in enumerate(fd for fd in fds if re_mf.search(fd['fpath'])):
        fd['_id'] = str(fd['_id'])
        if i % CHUNK_SIZE == 0:
            chunks.append(chunk)
            chunk = []
        chunk.append(fd)
    if chunk:
        chunks.append(chunk)
        
    return chunks

def manage_readers(sqlite_fpath, base_fpath, files_fpath, mongo_args):
    '''Manage a number of workers pulling fingerprints from files in parallel
    and return the number of music fingerprints read.

    The number of workers depends on the number of cpus on this machine.

    '''

    mhost, mport, mdn, mcn = mongo_args

    re_esc = re.compile(r'([\[\]\(\)\+\*\.\$\^\?])')
    mconn = None
    try:
        with sqlite3.connect(sqlite_fpath) as sc:
            sc.isolation_level = None
            sc.execute(FINGERPRINTS_TABLE_SQL)
            sr = sc.cursor()
            sr.execute('select oid from fingerprints')
            oids = list(objectid.ObjectId(r[0]) for r in sr.fetchall())
            sr.close()
            
            esc_fpath = re_esc.sub(r'\\\1', files_fpath)
            re_fpath = re.compile('^' + esc_fpath + '/' )
            mconn = pymongo.Connection(host=mhost, port=mport)
            music_db = mconn[mdn]
            files_cl = music_db[mcn]
            qry = {'fpath': re_fpath, 'complete': 1, 'fingerprinted': 0}
            n_files = files_cl.find(qry).count()
            if n_files == 0:
                return 0
            print('Will extract fingerprints of %d files in directory "%s"'
                  % (n_files, base_fpath + files_fpath))

            chunks = get_next_chunks(files_cl, qry, None, None)
            if not chunks:
                return 0
            n_chunks = len(chunks)

            que = multiprocessing.Queue(n_files)
            for chunk in chunks:
                que.put_nowait(chunk)

            workers = []
            for i in xrange(min(multiprocessing.cpu_count(), n_chunks)):
                p_conn, c_conn = multiprocessing.Pipe()
                p = multiprocessing.Process(
                    target=read_fprints,
                    args=(i, c_conn, que, base_fpath))
                p.name = str(i)
                p.start()
                n_chunks -= 1
                workers.append({
                    'proc': p,
                    'conn': p_conn,
                    'stopped': 0})

            n_alive = len(workers)
            n_read = 0
            rows = []
            while n_alive > 0:
                try:
                    for wr in workers:
                        if wr['stopped']:
                            continue
                        p = wr['proc']
                        if not p.is_alive():
                            wr['stopped'] = 1
                            n_alive -= 1
                            continue

                        cn = wr['conn']
                        if cn.poll(1.0):
                            try:
                                rows = cn.recv()
                                cn.send('ok')
                                if not rows:
                                    continue
                            except EOFError:
                                wr['stopped'] = 1
                                n_alive -= 1
                            else:
                                n_read += insert_fingerprints(sc, rows)
                                oids = list(objectid.ObjectId(fd['_id'])
                                            for fd, fp in rows)
                                files_cl.update(
                                    {'_id': {'$in': oids}}, 
                                    {'$set': {'fingerprinted': 1}},
                                    upsert=False,
                                    multi=True)
                                rows = []

                except (KeyboardInterrupt, SystemExit):
                    for wr in workers:
                        wr['conn'].close()
                        if not wr['stopped'] and wr['proc'].is_alive():
                            wr['proc'].terminate()
                    break

            if rows:
                n_read += insert_fingerprints(sc, rows)
                oids = list(objectid.ObjectId(fd['_id'])
                            for fd, fp in rows)
                files_cl.update(
                    {'_id': {'$in': oids}}, 
                    {'$set': {'fingerprinted': 1}},
                    upsert=False,
                    multi=True)
    finally:
        mconn = mconn and mconn.disconnect()

    return n_read

def parse_mongo_args(option, opt_str, val, prsr):
    '''Parse and return a tuple of:: 
    
      (host, port, db_name, collection_name)

    '''
    if '/' in val:
        host_port, db_coll = val.split('/', 1)
    else:
        host_port = val
        db_coll = '%s.%s' % (DEFAULT_MONGO_DN, DEFAULT_MONGO_CN)
    host = host_port
    port = DEFAULT_MONGO_PORT
    if ':' in host_port:
        host, port = host_port.split(':', 1)
        try:
            port = int(port)
        except ValueError:
            raise optparse.OptionValueError('%s port must be integral not %s'
                                            % (opt_str, port))
    if '.' not in db_coll:
        raise optparse.OptionValueError('%s requires db_name.coll_name not %s'
                                        % (opt_str, val))
    db_name, coll_name = db_coll.split('.', 1)
    setattr(prsr.values, option.dest, (host, port, db_name, coll_name))

def main(argv):
    '''%prog -s SQLITE_FPATH -m MONGO_HOST[:MONGO_PORT][/dbname.coll_name]
             -b BASE_PATH /music/file/path

    Extract music file fingerprints from mp3 and flac files under 
    /music/file/path and store in a sqlite3 database at SQLITE_FPATH or
    a MongoDB database at MONGO_HOST[:MONGO_PORT]/dbname
    '''

    oparser = optparse.OptionParser(conflict_handler='resolve', 
                                    usage=main.__doc__)
    oparser.add_option('-b', '--base-path', dest='base_path', type='string',
                       help='base path to remove from the actual absolute '
                       'filepaths to search the music MongoDB for original '
                       'music files.  BASE_PATH need not end with a "/".')
    oparser.add_option('-m', '--mongo', dest='mongo_args', type='string',
                       action='callback', callback=parse_mongo_args,
                       help='MONGO_HOST[:MONGO_PORT]/dbname.coll_name for '
                       'the input music files. MONGO_PORT defaults to 27017.')
    oparser.add_option('-s', '--sqlite', dest='sqlite_fpath', type='string',
                       help='absolute or relative filepath to a Sqlite3 '
                       'database.  If the database file does not exist, '
                       'it will be created. The table will be called '
                       '"fingerprints".')
    oparser.set_defaults(
        base_path=None,
        mongo_args=DEFAULT_MONGO_ARGS,
        sqlite_fpath=None)
    
    opts, args = oparser.parse_args(argv)
    
    if not opts.base_path:
        print('-b BASE_PATH is required')
        return errno.ENOENT
    base_path = opts.base_path

    if not opts.sqlite_fpath:
        print('-s SQLITE_FPATH is required')
        return errno.ENOENT
    sqlite_fpath = opts.sqlite_fpath

    mongo_args = opts.mongo_args
    
    if len(args) < 2:
        print('missing music file path')
        return errno.ENOENT
    music_fpath = filesys.posix_path(args[1], abspath=True)
    
    # support calling this function programmatically by cleaning up
    oparser.destroy()

    if not os.path.exists(music_fpath):
        print('ERROR: music file path "%s" does not exist or is not accessible'
              % music_fpath)
        return errno.ENOENT
    
    if not base_path and music_fpath.startswith(base_path):
        print('ERROR: base_path "%s" is empty or is not the base of the '
              'music file path "%s"' % (base_path, music_fpath))
        return errno.EINVAL

    bp_len = len(base_path)
    if not base_path.endswith('/'):
        base_path += '/'
        bp_len += 1
    files_fpath = music_fpath[bp_len:]
    
    n_read = manage_readers(sqlite_fpath, base_path, files_fpath, mongo_args)
    if n_read == 0:
        print('no files found for final music files path: "%s"' 
              % (files_fpath,))

    return 0

if __name__ == '__main__':
    main(sys.argv)
