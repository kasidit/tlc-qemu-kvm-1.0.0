#!/bin/bash

exeloc="/home/kasidit/TLCVM"

if [ ${1} = 'interleave' ]; 
then
    echo "${exeloc}/memory-server-interleave ${2} ${3} ${4} ${5} > ${6} " | at now
#    ./memory-server-interleave ${2} ${3}
elif [ ${1} = 'block' ]; 
then
    echo "${exeloc}/memory-server-block ${2} ${3} > ${4} " | at now
#    ./memory-server-block ${2} ${3}
else
    echo "usage: ./memory-server <type> <arg1> <arg2>\n"
    echo "  <type>: interleave or block\n"
    echo "  <arg1>: interleave --> number of servers\n"
    echo "          block --> prefix for your file\n"
    echo "  <arg2>: interleave --> port number\n"
    echo "          block --> port number\n"
    echo "Note: make sure the TLC_CHKPT_TYPE is\n"
    echo "      CYCLIC for interleave or BLOCK for block\n"
fi   
echo done
