#!/bin/sh -e
#
# This is not a general-purpose build script, but instead one specific to the Google oss-fuzz compile environment.
#
# https://google.github.io/oss-fuzz/getting-started/new-project-guide/#Requirements
#
# https://github.com/google/oss-fuzz/blob/master/infra/base-images/base-builder/README.md#provided-environment-variables
#
# We have to push to oss-fuzz CFLAGS into the waf ADDITIONAL_CFLAGS
# as otherwise waf's configure fails linking the first test binary
#
# CFLAGS are supplied by the caller, eg the oss-fuzz compile command
#
ADDITIONAL_CFLAGS="$CFLAGS"
export ADDITIONAL_CFLAGS
CFLAGS=""
export CFLAGS
LD="$CXX"
export LD

# $LIB_FUZZING_ENGINE is provided by the oss-fuzz "compile" command
#

./configure -C --without-gettext --enable-debug --enable-developer \
            --address-sanitizer --enable-libfuzzer \
	    --disable-warnings-as-errors \
	    --abi-check-disable \
	    --fuzz-target-ldflags="$LIB_FUZZING_ENGINE" \
	    --nonshared-binary=ALL LINK_CC="$CXX"

make -j

# Make a directory for the system shared libraries to be copied into
mkdir -p $OUT/lib

# We can't static link to all the system libs with waf, so copy them
# to $OUT/lib and set the rpath to point there.  This is similar to how
# firefox handles this.

for x in bin/fuzz_*
do
    cp $x $OUT/
    bin=`basename $x`
    ldd $OUT/$bin | cut -f 2 -d '>' | cut -f 1 -d \( | cut -f 2 -d  ' ' | xargs -i cp \{\} $OUT/lib/
    chrpath -r '$ORIGIN/lib' $OUT/$bin
done