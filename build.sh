#!/bin/sh
set -e
cc -Wall -Wextra -O2 -o ntype -Iinclude src/main.c src/utils.c
