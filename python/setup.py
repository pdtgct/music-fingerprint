'''
    Created by Peter Tanski on 27 June 2010.
    Copyright 2010 Zatisfi, LLC. MIT License, 2025
'''
import sys
from distutils.core import setup
from distutils.extension import Extension
from Cython.Distutils import build_ext

import os
import numpy
np_path = os.path.join(os.path.dirname(numpy.__file__), 'core/include')

# hack so we can build in the python/ directory while executing 
# while executing python/setup.py from $(TOPDIR)
os.chdir('python')

include_dirs = ('/usr/local/include', '../src', np_path)
library_dirs = ('/usr/lib', '/usr/local/lib', '../')
if sys.platform == 'darwin':
    include_dirs += ('/opt/local/include',)
    library_dirs += ('/opt/local/lib',)

ext_modules = [
    Extension(
      'musicfp',
      ['musicfp.pyx'],
      include_dirs=include_dirs,
      library_dirs=library_dirs,
      libraries=('chromaprint', 'fooid', 'avutil', 'avformat', 'avcodec', 'fingerprint')
      )
    ]

setup(
    name = 'musicfp',
    cmdclass = {'build_ext':build_ext},
    ext_modules=ext_modules
    )
