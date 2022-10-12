#!/bin/bash

## This script will convert a 16 channel .wav file into 8 separate stereo wav files.
## This mapping is for the Lilac Looper, which stores the channels 1-8 as all the left channels, and 9-16 as all the right channels.
## You must have install sox before running this.

if [ "$#" -ne 1 ]; then
    echo "Expecting one argument: the path to the wave file to split."
    exit 1
fi

WAVE=$(realpath ${1})
DIR=$(dirname ${WAVE})
NAME=$(basename ${WAVE} .wav)

if ! which sox >/dev/null 2>&1; then
    echo "sox is not installed."
    echo "Use your package manager to install sox, or visit https://sox.sourceforge.net/"
    exit 1
elif ! test -f ${WAVE}; then
    echo "WAVE file does not exist: ${WAVE}"
    exit 1
elif ! (sox --info ${WAVE} | grep "Channels" | grep ' 16$' >/dev/null); then
    sox --info ${WAVE}
    echo "WAVE file does not have 16 channels as expected."
    exit 1
fi

sox ${WAVE} ${DIR}/${NAME}_1.wav remix 1 9
sox ${WAVE} ${DIR}/${NAME}_2.wav remix 2 10
sox ${WAVE} ${DIR}/${NAME}_3.wav remix 3 11
sox ${WAVE} ${DIR}/${NAME}_4.wav remix 4 12
sox ${WAVE} ${DIR}/${NAME}_5.wav remix 5 13
sox ${WAVE} ${DIR}/${NAME}_6.wav remix 6 14
sox ${WAVE} ${DIR}/${NAME}_7.wav remix 7 15
sox ${WAVE} ${DIR}/${NAME}_8.wav remix 8 16
