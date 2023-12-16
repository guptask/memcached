#!/bin/sh

./autogen.sh
./configure CFLAGS=" --param=min-pagesize=0"
make -j"$(nproc)"
sudo make install
