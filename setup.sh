#!/bin/bash

set -e

git submodule update --init --recursive

bash "$(dirname "$0")/scripts/fetch_icons.sh"
