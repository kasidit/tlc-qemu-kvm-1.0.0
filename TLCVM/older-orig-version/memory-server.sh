#!/bin/bash

if [ ${1} = 'interleave' ]; 
then
    echo ./memory-server-interleave ${2} ${3}
    ./memory-server-interleave ${2} ${3}
elif [ ${1} = 'block' ]; 
then
    echo ./memory-server-block ${2} ${3}
    ./memory-server-block ${2} ${3}
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
