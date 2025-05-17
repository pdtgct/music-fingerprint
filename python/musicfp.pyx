from libc.stdint cimport *
from libc.string cimport *
from libc.stdlib cimport *
from libc.stdio cimport *
cimport cpython.exc as exc
cimport musicfp

import numpy as np
import struct
# special import for C-side compile-time information
cimport numpy as np
from numpy cimport *

cpdef init_ffmpeg():
    ffmpeg_init()

UINT8 = np.uint8
ctypedef np.uint8_t UINT8_t

DEF R_SIZE = 348
DEF DOM_SIZE = 66

cdef class Fingerprint:
    cdef public int errn
    cdef FPrint* fp
    
    def __cinit__(self):
        self.errn = 0
        self.fp = NULL

    def __dealloc__(self):
        if self.fp is not NULL:
            free_fprint(self.fp)

    cdef init_fingerprint(self, int cprint_len=0):
        cdef FPrint* fp = new_fprint(cprint_len)
        if fp is NULL:
            exc.PyErr_NoMemory()
        self.fp = fp

    cpdef c_fingerprint(self, char* fpath, int verbose):
        if self.fp is not NULL:
            free_fprint(self.fp)
        self.fp = get_fingerprint(fpath, &self.errn, verbose)

    cpdef match(self, Fingerprint o):
        cdef FPrint* o_fp = o.fp
        if self.fp is not NULL and o_fp is not NULL:
            return match_cpfm(self.fp, o_fp)

    cdef get_r(self):
        cdef np.ndarray[UINT8_t] r
        cdef Py_ssize_t i
        cdef npy_uint8* fp_r
        r = np.zeros(R_SIZE, dtype=UINT8)
        if self.fp is not NULL:
            fp_r = <npy_uint8*>(&self.fp.r[0])
            for i in range(R_SIZE):
                r[i] = fp_r[i]
            return r

    cdef set_r(self, np.ndarray[UINT8_t] ndarr):
        cdef Py_ssize_t i
        cdef uint8_t* r
        if self.fp is NULL:
            self.init_fingerprint()
        if self.fp is NULL:
            exc.PyErr_NoMemory()
        r = &self.fp.r[0]
        for i in range(R_SIZE):
            r[i] = ndarr[i]

    cdef get_dom(self):
        cdef np.ndarray[UINT8_t] dom
        cdef Py_ssize_t i
        cdef npy_uint8* fp_dom
        dom = np.zeros(DOM_SIZE, dtype=UINT8)
        if self.fp is not NULL:
            fp_dom = <npy_uint8*>(&self.fp.dom[0])
            for i in range(DOM_SIZE):
                dom[i] = fp_dom[i]
            return dom

    cdef set_dom(self, np.ndarray[UINT8_t] ndarr):
        cdef Py_ssize_t i
        cdef uint8_t* dom
        if self.fp is NULL:
            self.init_fingerprint()
        if self.fp is NULL:
            exc.PyErr_NoMemory()
        dom = &self.fp.dom[0]
        for i in range(DOM_SIZE):
            dom[i] = ndarr[i]

    cdef get_cprint(self):
        cdef np.ndarray[npy_int32] cp_arr
        cdef int32_t* cprint
        cdef Py_ssize_t i
        cdef Py_ssize_t cplen
        if self.fp is NULL:
            return None
        cplen = self.fp.cprint_len
        if cplen == 0:
            cp_arr = np.zeros(1, dtype=np.int32)
            return cp_arr
        cprint = <npy_int32*>self.fp.cprint
        cp_arr = np.zeros(cplen, dtype=np.int32)
        for i in range(cplen):
            cp_arr[i] = cprint[i]
        return cp_arr

    cdef set_cprint(self, np.ndarray[npy_int32] ndarr):
        cdef Py_ssize_t i
        cdef Py_ssize_t cprint_len = ndarr.size
        cdef int32_t* cprint = NULL
        cdef FPrint* nfp = NULL
        cdef size_t new_cp_size
        if self.fp is NULL:
            self.init_fingerprint(<int>cprint_len)
        if self.fp is NULL:
            exc.PyErr_NoMemory()
        if self.fp.cprint_len != <size_t>cprint_len:
            # we can't have a ndarr of size <= 0
            if (cprint_len < 1):
                cprint_len = 1
            nfp = <FPrint*>malloc(sizeof(FPrint)
                                  + (<size_t>(cprint_len-1) * sizeof(int32_t)))
            if nfp is NULL:
                exc.PyErr_NoMemory()
            memcpy(<void*>nfp, <void*>self.fp, sizeof(FPrint))
            free(self.fp)
            self.fp = nfp
        cprint = <int32_t*>self.fp.cprint
        for i in range(cprint_len):
            cprint[i] = <int32_t>ndarr[i]
        self.fp.cprint_len = <size_t>cprint_len

    cdef set_songlen(self, uint32_t songlen):
        if self.fp is NULL:
            self.init_fingerprint()
        self.fp.songlen = songlen

    cdef set_bit_rate(self, uint32_t bit_rate):
        if self.fp is NULL:
            self.init_fingerprint()
        self.fp.bit_rate = bit_rate

    cdef set_num_errors(self, uint32_t num_errors):
        if self.fp is NULL:
            self.init_fingerprint()
        self.fp.num_errors = num_errors

    property songlen:
        def __get__(self):
            if self.fp is not NULL:
                return self.fp.songlen

        def __set__(self, songlen):
            self.set_songlen(songlen)
          
    property bit_rate:
        def __get__(self):
            if self.fp is not NULL:
                return self.fp.bit_rate

        def __set__(self, bit_rate):
            self.set_bit_rate(bit_rate)
          
    property num_errors:
        def __get__(self):
            if self.fp is not NULL:
                return self.fp.num_errors

        def __set__(self, num_errors):
            self.set_num_errors(num_errors)

    property r:
        def __get__(self):
            return self.get_r()

        def __set__(self, ndarr):
            if not (isinstance(ndarr, np.ndarray) and
                    ndarr.dtype == np.uint8):
                raise TypeError('ndarr dtype must be numpy.uint8')
            if ndarr.size != R_SIZE:
                raise ValueError('ndarr size must be %d' % R_SIZE)
            self.set_r(ndarr)
    
    property dom:
        def __get__(self):
            return self.get_dom()

        def __set__(self, ndarr):
            if not (isinstance(ndarr, np.ndarray) and
                    ndarr.dtype == np.uint8):
                raise TypeError('ndarr dtype must be numpy.uint8')
            if ndarr.size != DOM_SIZE:
                raise ValueError('ndarr size must be %d' % DOM_SIZE)
            self.set_dom(ndarr)

    property cprint:
        def __get__(self):
            return self.get_cprint()

        def __set__(self, ndarr):
            if not (isinstance(ndarr, np.ndarray) and
                    ndarr.dtype == np.int32):
                raise TypeError('ndarr dtype must be numpy.int32')
            self.set_cprint(ndarr)

    def __reduce__(self):
        t = (
            struct.pack('<3I', self.songlen, self.bit_rate, self.num_errors),
            self.r.tostring(),
            self.dom.tostring(),
            self.cprint.tostring()
            )
        return (from_pickle, t)

    def __str__(self):
        return fprint_to_string(self.fp)

cpdef Fingerprint fingerprint(char* fpath, int verbose=0):
    cdef int errn = 0
    cdef FPrint* t_fp = NULL
    cdef Fingerprint fp = Fingerprint()
    cdef char* msg = NULL
    fp.errn = errn
    t_fp = get_fingerprint(fpath, &errn, verbose)
    if errn != 0 or t_fp == NULL:
        raise Exception('Error %d reading file %s' % (errn, fpath))
    fp.fp = t_fp
    return fp

def from_pickle(s_vars, s_r, s_dom, s_cprint):
    fp = Fingerprint()
    songlen, bit_rate, num_errors = struct.unpack('<3I', s_vars)
    # do this here as it initializes the FPrint to cprint size
    # otherwise it will be initialized with cprint[1], then
    # resized after (typically) cprint[948]
    fp.cprint = np.fromstring(s_cprint, dtype=np.int32)
    fp.songlen = songlen
    fp.bit_rate = bit_rate
    fp.num_errors = num_errors
    fp.r = np.fromstring(s_r, dtype=np.uint8)
    fp.dom = np.fromstring(s_dom, dtype=np.uint8)
    return fp

cpdef Fingerprint from_string(char* fp_str):
    cdef FPrint* t_fp = NULL
    fp = Fingerprint()
    t_fp = fprint_from_string(fp_str)
    if t_fp == NULL:
        raise Exception('invalid string format or memory error')
    fp.fp = t_fp
    return fp

def hamming_r(Fingerprint a not None, Fingerprint b not None):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    return hdist_r(fp_a.r, fp_b.r)

def hamming_dom(Fingerprint a not None, Fingerprint b not None):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    return hdist_dom(fp_a.dom, fp_b.dom)

def match_fingerprints(Fingerprint a not None, Fingerprint b not None):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    return match_fooid_fp(fp_a.r, fp_a.dom, fp_b.r, fp_b.dom)

def match_chromaf(Fingerprint a not None, Fingerprint b not None,
                  Py_ssize_t start=0, Py_ssize_t end=0):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    cdef int errn = 0
    cdef double val = 0.0
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    val = match_chroma(fp_a.cprint, fp_a.cprint_len,
                       fp_b.cprint, fp_b.cprint_len,
                       start, end,
                       &errn)
    if errn != 0:
        exc.PyError_NoMemory()
    return val

def match_chromap(Fingerprint a not None, Fingerprint b not None,
                  Py_ssize_t start=0, Py_ssize_t end=0):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    cdef double val = 0.0
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    val = match_chromab(fp_a.cprint, fp_a.cprint_len,
                        fp_b.cprint, fp_b.cprint_len)
    return val

def match_chromar(Fingerprint a not None, Fingerprint b not None,
                  Py_ssize_t start=0, Py_ssize_t end=0):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    cdef double val = 0.0
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    val = match_chromac(fp_a.cprint, fp_a.cprint_len,
                        fp_b.cprint, fp_b.cprint_len)
    return val

def match_chromatm(Fingerprint a not None, Fingerprint b not None,
                   Py_ssize_t start=0, Py_ssize_t end=0):
    cdef FPrint* fp_a = NULL
    cdef FPrint* fp_b = NULL
    cdef double val = 0.0
    fp_a = a.fp
    fp_b = b.fp
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    val = match_chromat(fp_a.cprint, fp_a.cprint_len,
                        fp_b.cprint, fp_b.cprint_len)
    return val

def union(Fingerprint a not None, Fingerprint b not None):
    cdef FPrint* fp_u = NULL
    cdef FPrint* fp_a = a.fp
    cdef FPrint* fp_b = b.fp
    cdef size_t u_cprint_len = 0
    if fp_a is NULL or fp_b is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    if fp_a == fp_b:
        raise ValueError("cannot union a Fingerprint with itself")
    nfp = Fingerprint()
    if fp_a.cprint_len > fp_b.cprint_len:
        u_cprint_len = fp_a.cprint_len
        fp_u = new_fprint(u_cprint_len)
        if fp_u is NULL:
            exc.PyErr_NoMemory()
    else:
        u_cprint_len = fp_b.cprint_len
        fp_u = new_fprint(u_cprint_len)
        if fp_u is NULL:
            exc.PyErr_NoMemory()
    memset(<void*>fp_u.cprint, 0, u_cprint_len * sizeof(int32_t))
    fprint_merge(<FPrintUnion*>fp_u, fp_a, fp_b)
    nfp.fp = fp_u
    return nfp

def match_fprint_union(Fingerprint a not None, Fingerprint u not None):
    cdef FPrint* fp_a = a.fp
    cdef FPrint* fp_u = u.fp
    if fp_a is NULL or fp_u is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    if fp_a == fp_u:
        return 1.0
    return match_fprint_merge(fp_a, <FPrintUnion*>fp_u)

def match_unions(Fingerprint u1 not None, Fingerprint u2 not None):
    cdef FPrint* fp_u1 = u1.fp
    cdef FPrint* fp_u2 = u2.fp
    if fp_u1 is NULL or fp_u2 is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    if fp_u1 == fp_u2:
        return 1.0
    return match_merges(<FPrintUnion*>fp_u1, <FPrintUnion*>fp_u2)

def try_match_unions(
    Fingerprint u1 not None,
    Fingerprint u2 not None,
    Fingerprint a not None):
    cdef FPrint* fp_u1 = u1.fp
    cdef FPrint* fp_u2 = u2.fp
    cdef FPrint* fp_a = a.fp
    if fp_u1 is NULL or fp_u2 is NULL or fp_a is NULL:
        raise ValueError("One or both Fingerprints have not been initialized")
    if fp_u1 == fp_u2 == fp_a:
        return 1.0
    return try_match_merges(<FPrintUnion*>fp_u1, <FPrintUnion*>fp_u2, fp_a)

init_ffmpeg()
