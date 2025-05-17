Chromaprint
===========

Dependencies
------------

The library itself only depends on a FFT library, which at the moment can
be either FFmpeg [1] (at least r22291, 0.6 is fine) or FFTW3 [2]. See the
next section for details.

The tools included in the package require FFmpeg (can be older), TagLib [3]
and Boost Filesystem [4].

In order to build the test suite, you will need the Google Test library [5].

[1] http://www.ffmpeg.org/
[2] http://www.fftw.org/
[3] http://developer.kde.org/~wheeler/taglib.html
[4] http://www.boost.org/
[5] http://code.google.com/p/googletest/

FFT Library
-----------

Chromaprint can use two FFT libraries, FFmpeg or FFTW3. FFmpeg is preffered,
as it's a little faster for our purposes and it's LGPL-licensed, so it
doesn't impact the license of Chromaprint. The FFT interface was added only
recently though, so it might not be available in Linux distributions yet.
FFTW3 can be used in this case, but this library is released under the GPL
license, which makes also the resulting Chromaprint binary GPL licensed.

If you run simple `cmake .`, it will try to find both FFmpeg and FFTW3 and
select the first one it finds. If you have new FFmpeg installed in a separate
location, you can let CMake know using the `FFMPEG_ROOT` option:

$ cmake -DFFMPEG_ROOT=/path/to/local/ffmpeg/install .

If you have new FFmpeg installed, but for some reason prefer to use FFTW3, you
can use the `WITH_FFTW3` option:  

$ cmake -DWITH_FFTW3=ON .

There is also a `WITH_AVFFT` option, but the script will select the FFmpeg FFT
automatically if it's available, so it shouldn't be necessary to use it.

Unit Tests
----------

The test suite can be built and run using the following commands:

$ cmake -DBUILD_TESTS=ON .
$ make check

Standing on the Shoulder of Giants
----------------------------------

I've learned a lot while working on this project, which would not be possible
without having information from past research. I've read many papers, but the
concrete ideas implemented in this library are based on the following papers:

 * Yan Ke, Derek Hoiem, Rahul Sukthankar. Computer Vision for Music
   Identification, Proceedings of Computer Vision and Pattern Recognition,
   2005. http://www.cs.cmu.edu/~yke/musicretrieval/

 * Frank Kurth, Meinard Müller. Efficient Index-Based Audio Matching, 2008.
   http://dx.doi.org/10.1109/TASL.2007.911552

 * Dalwon Jang, Chang D. Yoo, Sunil Lee, Sungwoong Kim, Ton Kalker.
   Pairwise Boosted Audio Fingerprint, 2009.
   http://dx.doi.org/10.1109/TIFS.2009.2034452

My Notes
========

BOTH:
* get googletest (for testing):
  wget http://code.google.com/p/googletest/downloads/detail?name=gtest-1.5.0.tar.bz2
  configure .. make .. sudo make install

on Darwin:
* sudo port install:
  libpng 
  pngpp
* cmake -DFFMPEG_ROOT=/opt/local -DBUILD_TESTS=ON -DMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=ON .
on Linux:
* apt-get install or get and build
  get pngpp from http://savannah.nongnu.org/projects/pngpp/ and build
  libavcodec-dev
  libavdevice-dev
  libavformat-dev
  libavutil-dev
  libavfilter-dev
  libpostproc-dev
  libswscale-dev
  libboost-all-dev
  libgtest-dev
* cmake -DFFMPEG_ROOT=/usr -DBUILD_TESTS=ON -DMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=ON .
* make
* make check (runs tests, requires pngpp, above)

To make the tools (fpcollect, fpgen; *not* fpeval, it is broken):

* cmake -DMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=ON .
* make
