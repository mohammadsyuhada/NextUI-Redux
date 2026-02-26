#!/bin/sh

cd $(dirname "$0")

./portmaster.elf &> "$LOGS_PATH/portmaster.txt"
