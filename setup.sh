#!/bin/bash

set -e

# Download dependency cloner
mkdir -p dependencies/work-dependency-cloner
git clone https://github.com/5cript/work-dependency-cloner.git dependencies/work-dependency-cloner || echo "Dependency cloner already cloned."

# Create a "build dir"
mkdir -p build/dependency-cloner
cmake -S dependencies/work-dependency-cloner -B build/dependency-cloner
cmake --build build/dependency-cloner
