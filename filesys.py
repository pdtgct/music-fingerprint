'''
    filesys.py
    file system utilities, particularly `posix_path` and `posix_join`

    Created by Peter Tanski on 27 June 2010.
    Copyright 2010 Zatisfi, LLC. MIT License, 2025
'''

import datetime
from itertools import imap, ifilter
from operator import itemgetter
import os
import re
import stat
import time

try:
    import mx.Tools
except:
    def exists(fn_find, iterable):
        for itm in iterable:
            if fn_find(itm):
                return True
        return False
    def index(fn_find, iterable):
        i = 0
        for itm in iterable:
            if fn_find(itm):
                return i
            i += 1
        raise ValueError('condition is false for all items in sequence')


__docformat__ = 'reStructuredText'


def norm_path(filepath):
    '''Return a normalized path with symbolic links, variables, and
    user variable (``~``) expanded and redundant path separators removed.
    '''
    ret_path = os.path.realpath(
                  os.path.normpath(
                     os.path.expandvars(
                        os.path.expanduser(filepath))))
    return ret_path

re_dirsep = re.compile(r'[\\/]+')
def posix_path(filepath, abspath=False, splitdrive=False, keepdrive=False):
    '''Return ``filepath`` converted from windows ``'\\'`` pathsep
    to posix ``'/'``, with the drive removed (if any).

    The resulting path is normalized, with all variables expanded and 
    duplicate path separators removed.

    ``abspath``
        if the first path in ``*pathelems`` is not absolute (does not begin
        with a ``"/"`` or ``"\\"``), return the absolute path to the
        directory. This uses ``os.path.abspath`` which prepends the
        absolute path to the current working directory (it will not find
        the UNC hostname for you)

    ``splitdrive``
        Return the path with the drive letter removed and the drive name
        converted to UNC format. (``splitdrive`` implies ``abspath``.)
        Windows only: requires ``win32wnet`` ``module``.
        On non-Windows systems this option has the same effect as
        ``os.path.abspath``

        WARNING: for local drives that are not mapped as network drives,
        such as D: mapped to a CD-ROM, ``splitdrive=True`` will return the
        wrong absolute path: ``"D:/Temp" --> "/Temp"``.  To avoid this,
        use ``keepdrive`` (ignored on non-Windows systems)

    ``keepdrive``
        optional, default False; if True, and ``splitdrive`` is ``True``,
        replace a remote host with the UNC host, i.e.::

          Y:\\Peter --> //pfa-ct-intex01/ModelOutput/Peter

        and preserve a local drive::

          C:\\Temp --> C:/Temp

        On non-Windows system this option is ignored

    '''
    url_st = ''
    if filepath.startswith('\\\\') or filepath.startswith('//'):
        url_st = '/'
    ret_path = norm_path(filepath)
    if (abspath or splitdrive) and url_st:
        ret_path = os.path.abspath(ret_path)
    return url_st + re_dirsep.sub('/', ret_path)

def posix_join(*pathelems, **kwargs):
    '''Super ``posix_path``: ``performs posix_path(os.path.join(*pathelems))``,
    with the resulting path normalized and duplicate path separators removed.

    This function accepts the same keyword arguments as `posix_path`:

    ``abspath``
        if the first path in ``*pathelems`` is not absolute (does not begin 
        with a ``"/"`` or ``"\\"``), return the absolute path to the directory.
        This uses ``os.path.abspath`` which prepends the absolute path in the
        current working directory (it will not find the UNC hostname for you)

    ``splitdrive``
        Return the path with the drive letter replaced with the UNC mapping
        to a network drive and local drives removed. Windows only.
        On non-Windows systems this option has no effect

    ``keepdrive``
        Return a local drive prepended to a drive name.  In combination with
        ``splitdrive``, this option will effectively resolve mapped drives to
        their network UNC paths but keep local drives local.

        This is a separate option for compatibility with web posix but 
        requires ``urllib.pathname2url`` for web interfaces, i.e.,
        ``"file://C:/"`` is a nonstandard url, so if you are formatting the
        path name for a URL link, use::
        
            "file://" + urllib.pathname2url(filesys.posix_join(
                                                   "C:/path1",
                                                   "path2",
                                                   splitdrive=True,
                                                   keepdrive=True)),

        which will result in a valid url of::

            "file://C|/path1/path2"

        but without ``urllib.pathname2url`` would create a bad url::

            "file://C:/path1/path2"

    '''
    return posix_path(os.path.join(*pathelems), 
                      abspath=kwargs.get('abspath', None), 
                      splitdrive=kwargs.get('splitdrive', None),
                      keepdrive=kwargs.get('keepdrive', None))

def find_files(directory, filepats, maxdepth=-1,
               hierarchy=None, exclude_dirs=None,
               limit=-1, sort_order=0):
    '''Return a list of absolute paths to any existing files
    in ``directory`` or its subdirectories that match ``filepat``
    or ``[]``.  In any case, do not include directories in
    ``exclude_dirs``.

    Filepaths returned will follow the conventions on the local system.

    ``directory``
       Root directory to search

    ``filepats``
        Filepatterns to match. Filepatterns may be regular
        expressions, strings, or callable objects that return a value
        that evaluates to a boolean

    ``maxdepth``
       Maximum depth to search for each subdirectory.
       Values:
       
           ``== 0``
               only the top directory will be searched;
           ``>  0``
               this function will recurse each subdirectory to the
               limit of ``maxdepth`` directories (counting from 0); or,
           ``<  0``
               unlimited

    ``hierarchy``
       A sequence type containing strings (subdirectory names), 
       re patterns, or callables that accept a single string
       argument and return a value that evaluates to a boolean.

       This defines a hierarchy of directories to follow _before_
       searching for files.  For example, you might pass in a list
       of ``["\\d{4}", "\\d{1,2}_\\d{4}", "Report"]`` if you want to
       search all "Report" directories in a hierarchy of
       ``"/Year/month_Year/Report"``

       If given, ``directory`` serves as the base directory of the
       ``hierarchy``

    ``exclude_dirs``
       An iterable of strings (subdirectory names), re patterns,
       or callables that accept a single string argument and return 
       a value that evalutes to a boolean.
       If patterns, the patterns may be regular expressions or
       ``re``.``pattern`` objects; these will by default ``re``.``search`` the
       directory names.  If you want to match, pass the 
       ``[re_pattern]``.``match`` method as the ``exclude_dirs`` element.
       For further reference see `utils.txtutils.mk_repat`

    ``limit``
       Integral number of files to return.
       Values:
       
           ``== 0``
               return no files (get_filename_dates is a no-op)
           ``>  0``
               only ``limit`` files will be returned, taken from the
               top number, based on ``sort_order``
           ``<  0``
               unlimited

    ``sort_order``
       Integral value roughly corresponding to ``cmp()``
       Returns:
       
           ``== 0``
               do not sort files; return in order found (directories 
               are searched in lexical order)
           ``>  0``
               sort ascending
           ``<  0``
               sort descending
           
       Values are sorted according to their base filename

    '''
    if not directory:
        directory = '.'
    def remove_hdir(h_dir, dirs):
        m_hdir = lambda d: not h_dir(d)
        while 1:
            try:
                ix = index(m_hdir, dirs)
            except ValueError:
                break
            else:
                del dirs[ix]
    if not hierarchy:
        hierarchy = []
    else:
        hierarchy = mk_repats(hierarchy)
    def remove_excl(e_dir, dirs, rdirs):
        m_edir = lambda d: e_dir(d)
        ix = 0
        ndirs = len(dirs)
        while ix < ndirs:
            dph = rdirs[ix]
            if e_dir(dph):
                del dirs[ix]
                del rdirs[ix]
                ndirs -= 1
            else:
                ix += 1
    if not exclude_dirs:
        exclude_dirs = []
    else:
        exclude_dirs = mk_repats(exclude_dirs)
    if not filepats:
        filepats = [mk_repat('.*')]
    elif isinstance(filepats, basestring):
        filepats = [mk_repat(filepats)]
    else:
        filepats = mk_repats(filepats)
    def in_filepats(fn):
        return exists(lambda mtch_fn: mtch_fn(fn), filepats)
    directory = norm_path(directory)
    if limit == 0:
        return []
    ret_files = []
    start_depth = len(re_dirsep.split(directory))
    hier_len = len(hierarchy)
    if maxdepth >= 0:
        maxdepth += start_depth + hier_len

    for root, dirs, files in os.walk(directory, topdown=True):
        dir_depth = len(re_dirsep.split(root))
        hier_ix = dir_depth - start_depth
        fpdirs = list('/'.join((root, d)) for d in dirs)
        if hier_ix < hier_len:
            h_dir = hierarchy[hier_ix]
            remove_hdir(h_dir, dirs)
            for ed in exclude_dirs:
                remove_excl(ed, dirs, fpdirs)
            continue
        if maxdepth >= 0 and dir_depth > maxdepth:
            continue
        mfiles = ifilter(in_filepats, files)
        ret_files.extend(imap(lambda fl: os.path.join(root, fl), mfiles))
        for ed in exclude_dirs:
            remove_excl(ed, dirs, fpdirs)
    limit = limit < 0 and len(ret_files) or limit
    reverse = sort_order < 0 and True or False
    return sorted(ret_files,
                  key=os.path.basename,
                  reverse=reverse)[:limit]

def get_filestat_dates(directory, filepats, time_typ='modified',
                       maxdepth=0, exclude_dirs=None, limit=-1,
                       sort_order=-1):
    '''Return the most recent files found based on the ``time_typ``, which
    may be one of ``('modified', 'created', 'accessed')``.  This recurses
    subdirectories, just like find_files(), but does not check the times
    of directories (``stat.S_ISDIR``).  The return value is a list of tuples:
    ``[(datetime.datetime, absolute_filename)]``.

    Note that `sort_order` defaults to ``-1`` here because the most common use
    of this function is to return the most recent files.

    '''
    if time_typ == 'modified':
        st_typ = 'st_mtime'
    elif time_typ == 'accessed':
        st_typ = 'st_atime'
    elif time_typ == 'created':
        st_typ = 'st_ctime'
    else:
        raise ValueError(('time_typ (%s) must be "modified", "created" or '
                          '"accessed"') % time_typ)
    def get_filetime(abs_flnm):
        st = os.stat(abs_flnm)
        if not stat.S_ISDIR(st.st_mode):
            file_time = getattr(st, st_typ)
            return (datetime.datetime.fromtimestamp(file_time), abs_flnm)
        return None
    files = find_files(directory, filepats, maxdepth, exclude_dirs)
    if not files:
        return []
    tm_files = filter(lambda f: f, imap(get_filetime, files))
    limit = limit < 0 and len(tm_files) or limit
    reverse = sort_order < 0 and True or False
    return sorted(tm_files, key=itemgetter(0), reverse=reverse)[:limit]

# misc utils

if not hasattr(re, '_pattern_type'):
    re._pattern_type = type(re.compile(''))

def mk_repats(srexps, re_match=False):
    '''Return a list re pattern objects from ``srexps``, if possible.

    If a ``srexp`` in ``srexps`` is not a basestring and callable, it will be
    returned unchanged.  If a ``srexp`` fails to compile to a regular 
    expression, a ``re.error`` will be raised.

    '''
    return map(lambda sr: mk_repat(sr, re_match), srexps)

def mk_repat(srexp, re_match=False):
    '''Return a callable that when invoked will match or search a compiled
    ``re Pattern`` object from ``srexp``

    ``srexp``
        ``string`` compilable to a regular expression without being escaped
        through ``re.escape()``; a compiled re ``Pattern object`` or
        ``callable`` that accepts a single string as an argument.
        (This function does not check the argspec of the ``callable``)

    ``re_match``
        If True and the callable is a re pattern object, when called
        the returned object will invoke ``re_obj.match()``;
        if false (default) the object will invoke ``re_obj.search()``

    If ``srexp`` is not a basestring, and it is callable or an re.Pattern
    object it will be returned unchanged; otherwise a ``TypeError`` will
    be raised.

    '''
    match = re_match and 'match' or 'search'
    if isinstance(srexp, basestring):
        rp = re.compile(srexp)
        return getattr(rp, match)
    elif isinstance(srexp, re._pattern_type):
        return getattr(srexp, match)
    elif callable(srexp):
        return srexp
    else:
        raise TypeError(('%s type: %s is not a basestring, SRE_Pattern object,'
                         ' or callable') % (srexp, type(srexp).__name__))
