#!/bin/sh

PREFIX=${PREFIX:=~/prefix}
PYTHON=${PREFIX}/bin/python

$PYTHON ./setup.py clean
$PYTHON ./setup.py build_ext -I${PREFIX}/include -L${PREFIX}/lib -R${PREFIX}/lib || exit 1
$PYTHON ./setup.py install --prefix=${PREFIX}
