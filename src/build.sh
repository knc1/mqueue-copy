#!/bin/sh

PATH=/opt/cross/bin/:$PATH
CROSS='arm-linux-gnueabi-'
${CROSS}gcc -O2 -mfloat-abi=softfp -mfpu=vfp3 -march=armv7-a -mcpu=cortex-a8 -o mq_copy mq_copy.c -lrt
${CROSS}strip --strip-unneeded mq_copy
