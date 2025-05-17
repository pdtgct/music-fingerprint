/*
 *  pgfprint.h
 *  support library for PostgreSQL
 *
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
 */

#ifndef _PGFPRINT_H
#define _PGFPRINT_H

// always include postgres.h first, before even system headers
#include "postgres.h"
#include "fmgr.h"

Datum fprint_in(PG_FUNCTION_ARGS);
Datum fprint_out(PG_FUNCTION_ARGS);
Datum fprint_cmp(PG_FUNCTION_ARGS);

int rcmp_matches(const void *match1, const void *match2);

#endif
