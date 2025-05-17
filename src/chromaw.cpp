/*
 *  chromaw.cpp
 *  wrapper for chromaprint to allow us to write
 *  fplib using C99 features not available in C++
 *
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
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

#ifdef __cplusplus
extern "C"
{
#endif

#include <errno.h>
#include <stdlib.h>

#include "chromaw.h"

#ifdef __cplusplus
}
#endif

#include <chromaprint/fingerprinter.h>

using namespace Chromaprint;

ChromaFingerprinter chroma_init(int sample_rate, int num_channels)
{
    Fingerprinter *cpr = new Fingerprinter();
    try
    {
        cpr->Fingerprinter::Init(sample_rate, num_channels);
    }
    catch (...)
    {
        return NULL;
    }
    return static_cast<ChromaFingerprinter>(cpr);
}

int chroma_feed(ChromaFingerprinter cpr, int16_t *data, int32_t len)
{
    if (len == 0)
        return 0;
    try
    {
        (static_cast<Fingerprinter *>(cpr))->Fingerprinter::Consume(data, len);
    }
    catch (...)
    {
        return -1;
    }
    return 0;
}

int32_t *chroma_calculate(ChromaFingerprinter cpr,
                          int *errn,
                          size_t *outlen)
{
    std::vector<int32_t> cpr_fp;
    int32_t *cprint = NULL;
    size_t cpr_len = 0;

    *outlen = 0;
    try
    {
        cpr_fp = (static_cast<Fingerprinter *>(cpr))->Fingerprinter::Calculate();
    }
    catch (...)
    {
        *errn = -1;
        return NULL;
    }

    cpr_len = static_cast<size_t>(cpr_fp.size());
    if (cpr_len == 0)
    {
        *errn = 1;
        return NULL;
    }

    cprint = (int32_t *)calloc(cpr_len, sizeof(*cprint));
    if (!cprint)
    {
        *errn = ENOMEM;
        return NULL;
    }
    for (size_t j = 0; j < cpr_len; j++)
    {
        cprint[j] = cpr_fp[j];
    }

    *errn = 0;
    *outlen = cpr_len;
    return cprint;
}

void chroma_destroy(ChromaFingerprinter cpr)
{
    Fingerprinter *fcpr = static_cast<Fingerprinter *>(cpr);
    if (fcpr)
    {
        fcpr->Fingerprinter::~Fingerprinter();
        cpr = NULL;
    }
}
