from libc.stdint cimport *

DEF R_SIZE = 348
DEF DOM_SIZE = 66

cdef extern from "fplib.h" nogil:
    ctypedef struct FPrint:
        size_t    cprint_len
        uint32_t  songlen
        uint32_t  bit_rate
        uint32_t  num_errors
        uint8_t   r[R_SIZE]
        uint8_t   dom[DOM_SIZE]
        int32_t   cprint[1]

    ctypedef struct FPrintUnion:
        size_t    cprint_len
        uint32_t  min_songlen
        uint32_t  bit_rate
        uint32_t  max_songlen
        uint8_t   r[R_SIZE]
        uint8_t   dom[DOM_SIZE]
        int32_t   cprint[1]

    void ffmpeg_init()
    FPrint* new_fprint(int cprint_len)
    void free_fprint(FPrint* fp)
    FPrint* get_fingerprint(char* filename, int* error, int verbose)
    unsigned int hdist_r(uint8_t* r_a, uint8_t* r_b)
    unsigned int hdist_dom(uint8_t* dom_a, uint8_t* dom_b)
    double match_fooid_fp(uint8_t* r_a, uint8_t* dom_a, 
                          uint8_t* r_b, uint8_t* dom_b)
    double match_chroma(int32_t* cp1, size_t cp1_len,
                        int32_t* cp2, size_t cp2_len,
                        size_t start, size_t end,
                        int* error)
    double match_chromab(int32_t* cp1, size_t cp1_len,
                         int32_t* cp2, size_t cp2_len)
    double match_chromac(int32_t* cp1, size_t cp1_len,
                         int32_t* cp2, size_t cp2_len)
    double match_chromat(int32_t* cp1, size_t cp1_len,
                         int32_t* cp2, size_t cp2_len)
    double match_cpfm(FPrint* a, FPrint* b)
    void fprint_merge(FPrintUnion* u, FPrint* a, FPrint* b)
    void fprint_merge_one(FPrintUnion* u, FPrint* a)
    float match_fprint_merge(FPrint* a, FPrintUnion* u)
    float match_merges(FPrintUnion* u1, FPrintUnion* u2)
    float try_match_merges(FPrintUnion* u1, FPrintUnion* u2, FPrint* a)
    char* fprint_to_string(FPrint* fp)
    FPrint* fprint_from_string(char* fp_str)
