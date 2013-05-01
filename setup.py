#!/usr/bin/env python

from setuptools import setup, Extension

VERSION = '0.5'
DESCRIPTION = 'libucoev bindings - I/O-scheduled coroutines'
LONG_DESCRIPTION = """
    libucoev, ucoev Python threading model and the present module 
    are the results of implementing event- and cooperatively-
    scheduled coroutines on top of ucontext_t and associated libc 
    functions.
"""

CLASSIFIERS = filter(None, map(str.strip,
"""
Development Status :: 3 - Alpha
Intended Audience :: Developers
License :: OSI Approved :: MIT License
Natural Language :: English
Programming Language :: Python
Operating System :: POSIX
Topic :: Software Development :: Libraries :: Python Modules
""".splitlines()))

REPOSITORY="https://coev.googlecode.com/hg/"

daext = Extension(
    name='_coev', 
    sources=['modcoev.c'], 
    undef_macros=['NDEBUG'],
    libraries=['ucoev']
    )

setup(
    name="coev",
    version=VERSION,
    description=DESCRIPTION,
    long_description=LONG_DESCRIPTION,
    classifiers=CLASSIFIERS,
    maintainer="Alexander Sabourenkov",
    maintainer_email="llxxnntt@gmail.com",
    url=REPOSITORY,
    license="MIT License",
    platforms=['any'],
    test_suite='nose.collector',
    download_url=REPOSITORY,
    ext_modules=[daext],
    packages=['coev'],
    zip_safe=False
)
