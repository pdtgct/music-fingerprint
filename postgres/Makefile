# Makefile for libpgfingerprint (fprint datatype for PostgreSQL)
#
# Linux Note: most of the features here require the aptitude package:
#  postgresql-server-dev-8.4 (8.4: our current version)
#
# Created by Peter Tanski on 27 June 2010.
# Copyright 2010 Zatisfi, LLC. MIT License, 2025
#

OS = $(shell uname -s)
OSX_VERS = $(shell sw_vers | grep ProductVersion | sed 's/.*10.\([56]\).*/\1/')

CC = gcc
PG_LIBS := -L.. -L/usr/local/lib -lfingerprint
ifeq ($(OS),Darwin)
	PG_CPPFLAGS := -std=c99 -isysroot /Developer/SDKs/MacOSX10.$(OSX_VERS).sdk -I/usr/local/include -I../src
	SHLIB_LINK = -flat_namespace -Wl,-syslibroot,/Developer/SDKs/MacOSX10.$(OSX_VERS).sdk $(PG_LIBS)
else
	PG_CPPFLAGS := -std=c99 -I/usr/local/include -I../src
	SHLIB_LINK = -shared  $(PG_LIBS)
endif

ifdef DEBUG
	PG_CPPFLAGS += -DDEBUG=1
endif

OBJS = pgfprint.o

MODULES = pgfprint
DATA_built = pgfprint.so
DATA = pgfprint.sql
MODULE_big = pgfprint

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifeq ($(OS),Darwin)
	ifeq ($(OSX_VERS),6)
		override CFLAGS := $(subst -arch i386,,$(CFLAGS))
	else
		override CFLAGS := $(subst -arch x86_64,,$(CFLAGS))
	endif
	ifdef DEBUG
		override CFLAGS := $(subst -Os,-g,$(subst -O3,-g,$(CFLAGS)))
	endif
else
# the standard apt install uses both -g and -O2
	ifdef DEBUG
		override CFLAGS := $(subst -O2,-g,$(subst -O3,-g,$(CFLAGS)))
	else
		override CFLAGS := $(subst -g ,,$(CFLAGS))
	endif
endif
