# Created by Peter Tanski on 27 June 2010.
# Copyright 2010 Zatisfi, LLC. MIT License, 2025

SHELL=/bin/bash

CC = gcc
LD = ld
# if using g++, need to add -D__STDC_CONSTANT_MACROS for UINT64_C
# see http://lists.mplayerhq.hu/pipermail/libav-user/2010-July/004934.html
CXX = g++
CFLAGS := -std=gnu99 -O3 -Wall -Wpointer-arith -funroll-loops
CXXFLAGS = -O3 -D__STDC_CONSTANT_MACROS 
CPPFLAGS := -I/usr/local/include -I. -I./src -I/usr/local/include/libfooid
LDFLAGS := -L. -L/usr/local/lib -lm
CHROMA_LIBS := -lchromaprint
FP_LIBS := -lavutil -lavformat -lavcodec -lfooid -lchromaw
BIN_LIBS := -lfingerprint
OS = $(shell uname -s)
OSX_VERS = $(shell sw_vers | grep ProductVersion | sed 's/.*10.\([56]\).*/\1/')
ifeq ($(OS),Darwin)
	SHARED = -dynamiclib -fPIC
	CPPFLAGS += -I/opt/local/include
	LDFLAGS += -L/opt/local/lib
	DYLIB_SUF = dylib
	ROOT_GROUP = wheel
	ifeq ($(OSX_VERS),6)
		ARCHFLAGS = ARCHFLAGS="-arch x86_64"
	else
		ARCHFLAGS = ARCHFLAGS="-arch i386"
	endif
else
	SHARED = -shared -fPIC
	CPPFLAGS += -I/usr/include
	LDFLAGS += -L/usr/lib
	DYLIB_SUF = so
	ROOT_GROUP = root
endif

WD := $(shell pwd)

FPLIB := libfingerprint.$(DYLIB_SUF)
CHROMAWLIB := libchromaw.$(DYLIB_SUF)

ifeq ($(OS),Darwin)
  ifeq ($(OSX_VERS),6)
    RPATH_EXEC := install_name_tool \
    -change $(CHROMAWLIB) /usr/local/lib/$(CHROMAWLIB) \
    -change libfooid.dylib /usr/local/lib/libfooid.dylib \
    -rpath $(WD):/usr/local/lib /usr/local/lib /usr/local/lib/$(FPLIB)
  else
    RPATH_EXEC := install_name_tool \
    -change $(CHROMAWLIB) /usr/local/lib/$(CHROMAWLIB) \
    -change libfooid.dylib /usr/local/lib/libfooid.dylib \
    /usr/local/lib/$(FPLIB)
  endif
endif

all : fingerprint $(FPLIB)

install : 
	- rm /usr/local/lib/$(FPLIB)
	cp $(CHROMAWLIB) /usr/local/lib/
	chown root:$(ROOT_GROUP) /usr/local/lib/$(CHROMAWLIB)
	cp $(FPLIB) /usr/local/lib/
	$(RPATH_EXEC)
	chown root:$(ROOT_GROUP) /usr/local/lib/$(FPLIB)

fingerprint : src/fingerprint.c $(FPLIB) $(CHROMAWLIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(BIN_LIBS) $< -o $@

$(FPLIB) : src/fplib.c $(CHROMAWLIB)
	$(CC) $(SHARED) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
	-Wl,-rpath,$(WD):/usr/local/lib $(FP_LIBS) $< -o $@

$(CHROMAWLIB) : src/chromaw.cpp
	$(CXX) $(SHARED) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $(CHROMA_LIBS) $< -o $@

src/fplib.c : src/fplib.h
src/fplib.h :
src/chromaw.cpp : src/chromaw.h
src/chromaw.h :

python : musicfp.so

musicfp.so : python/musicfp.pxd python/musicfp.pyx $(FPLIB)
	$(ARCHFLAGS) CFLAGS="-std=c99" python python/setup.py build
	find python/build -name musicfp.so -type f -exec cp "{}" . \;

src/fingerprint.c :
src/fplib.cpp :
python/musicfp.pxd :
python/musicfp.pyx :

clean :
	- rm src/fingerprint.o
	- rm fingerprint
	- rm $(FPLIB)
	- rm $(CHROMAWLIB)

uninstall :
	- rm /usr/local/lib/$(CHROMAWLIB)
	- rm /usr/local/lib/$(FPLIB)

clean-python :
	- rm -Rf python/build
	- rm musicfp.so

cleanall : clean clean-python

.PHONY : all python clean clean-python cleanall uninstall