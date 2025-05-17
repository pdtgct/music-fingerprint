/*
 *  chromaw.h
 *  wrapper for chromaprint to allow us to write 
 *  fplib using C99 features not available in C++
 *
 *  Created by Peter Tanski on 6 October 2010.
 *  Copyright 2010 Zatisfi, LLC. All rights reserved.
 *
 *  -- the code here is based on Chromaprint; the license is below --
 *
 *  Chromaprint -- Audio fingerprinting toolkit
 *  Copyright (C) 2010  Lukas Lalinsky <lalinsky@gmail.com>
 * 
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 * 
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *
 */

#ifndef _CHROMA_WRAPPER
#define _CHROMA_WRAPPER

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef void* ChromaFingerprinter;

ChromaFingerprinter chroma_init(int sample_rate, int num_channels);

int chroma_feed(ChromaFingerprinter cpr, int16_t* data, int32_t len);

int32_t* chroma_calculate(ChromaFingerprinter cpr,
                               int* errn,
                               size_t* outsize);

void chroma_destroy(ChromaFingerprinter cpr);

#ifdef __cplusplus
}
#endif

#endif
