#! /system/bin/sh
#
# Copyright 2016, The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

function help() {
    echo "USAGE: $0 [options] <input>"
    echo
    echo "OPTIONS:"
    echo "  -h        Show this help message."
    echo "  -o <file> Write output to file."
}

OUTPUT_FILE=""

while getopts "ho:" opt; do
    case "$opt" in
        h)
            help
            exit 0
            ;;
        o)
            OUTPUT_FILE=$OPTARG
            ;;
    esac
done

shift $((OPTIND-1))

if [[ "$#" -ne 1 ]]; then
  help
  exit -1
fi

INPUT_FILE=$1

if [[ -z "$OUTPUT_FILE" ]]; then
    OUTPUT_FILE="${INPUT_FILE%.*}.spv"
fi

KERNEL="${INPUT_FILE%.*}_k.spv"
KERNEL_TXT="${INPUT_FILE%.*}_k.spt"
WRAPPER="${INPUT_FILE%.*}_w.spt"
OUTPUT_TXT="${INPUT_FILE%.*}.spt"

eval rs2spirv $INPUT_FILE -o $KERNEL -wo $WRAPPER &&
eval spirv-dis $KERNEL --no-color -o $KERNEL_TXT &&
eval rs2spirv -o $OUTPUT_TXT -lk $KERNEL_TXT -lw $WRAPPER &&
eval spirv-as $OUTPUT_TXT -o $OUTPUT_FILE

#rm -f $INPUT_FILE $KERNEL $KERNEL_TXT $WRAPPER $OUTPUT_TXT

exit $?
