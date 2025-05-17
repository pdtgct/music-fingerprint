-------------------------------------------------------------------------------
--
-- fprint -- FPrint type for PostgreSQL
--
--  Created by Peter Tanski on 27 June 2010.
--  Copyright 2010 Zatisfi, LLC. MIT License, 2025
--
-------------------------------------------------------------------------------

-- where objects get created
SET search_path = public;

-- to uninstall, you should first remove dependent tables
-- then execute something like:
--   postgres=# DROP FUNCTION IF EXISTS fprint_in(cstring) CASCADE;

CREATE OR REPLACE FUNCTION fprint_in(cstring)
       RETURNS fprint
       AS '$libdir/pgfprint.so', 'fprint_in'
       LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION fprint_out(fprint)
       RETURNS cstring
       AS '$libdir/pgfprint.so', 'fprint_out'
       LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE fprint (
       internallength = variable,
       input = fprint_in,
       output = fprint_out,
       storage = extended,
       alignment = double
);

CREATE OR REPLACE FUNCTION fprint_consistent(internal, fprint, int4)
       RETURNS bool
       AS '$libdir/pgfprint.so', 'fprint_consistent'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_compress(internal)
       RETURNS internal
       AS '$libdir/pgfprint.so', 'fprint_compress'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_decompress(internal)
       RETURNS internal
       AS '$libdir/pgfprint.so', 'fprint_decompress'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_penalty(internal, internal, internal)
       RETURNS internal
       AS '$libdir/pgfprint.so', 'fprint_penalty'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_picksplit(internal, internal)
       RETURNS internal
       AS '$libdir/pgfprint.so', 'fprint_picksplit'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_union(internal, internal)
       RETURNS internal
       AS '$libdir/pgfprint.so', 'fprint_union'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_same(fprint, fprint, internal)
       RETURNS internal
       AS '$libdir/pgfprint.so', 'fprint_same'
       LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION fprint_cmp(fprint, fprint)
       RETURNS float8
       AS '$libdir/pgfprint.so', 'fprint_cmp'
       LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION fprint_eq(fprint, fprint)
       RETURNS bool
       AS '$libdir/pgfprint.so', 'fprint_eq'
       LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR = (
       leftarg = fprint,
       rightarg = fprint,
       procedure = fprint_eq,
       commutator = '=',
       negator = '<>',
       restrict = eqsel,
       join = eqjoinsel
);

CREATE OR REPLACE FUNCTION fprint_neq(fprint, fprint)
       RETURNS bool
       AS '$libdir/pgfprint.so', 'fprint_neq'
       LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <> (
       leftarg = fprint,
       rightarg = fprint,
       procedure = fprint_neq,
       commutator = '<>',
       negator = '=',
       restrict = eqsel,
       join = eqjoinsel
);

CREATE OR REPLACE FUNCTION fprint_match(fprint, fprint)
       RETURNS bool
       AS '$libdir/pgfprint.so', 'fprint_match'
       LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR ~= (
       leftarg = fprint,
       rightarg = fprint,
       procedure = fprint_match,
       commutator = '~=',
       restrict = eqsel,
       join = eqjoinsel
);

CREATE OPERATOR CLASS fprint_gist_ops
    DEFAULT FOR TYPE fprint USING GIST AS
        STORAGE fprint,
        OPERATOR   3  = (fprint, fprint),
        OPERATOR   6  ~= (fprint, fprint),
        OPERATOR  12  <> (fprint, fprint),
        FUNCTION   1  fprint_consistent (internal, fprint, int4),
        FUNCTION   2  fprint_union (internal, internal),
        FUNCTION   3  fprint_compress (internal),
        FUNCTION   4  fprint_decompress (internal),
        FUNCTION   5  fprint_penalty (internal, internal, internal),
        FUNCTION   6  fprint_picksplit (internal, internal),
        FUNCTION   7  fprint_same (fprint, fprint, internal);

-- Extra attribute functionality

CREATE OR REPLACE FUNCTION fprint_songlen(fprint)
       RETURNS int4
       AS '$libdir/pgfprint.so', 'fprint_songlen'
       LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION fprint_num_errors(fprint)
       RETURNS int4
       AS '$libdir/pgfprint.so', 'fprint_num_errors'
       LANGUAGE C IMMUTABLE STRICT;
