#!/usr/bin/env bash

set -e
set -x
shopt -s dotglob

readonly name="nlohmann_json"
readonly ownership="JSON For Modern C++ Upstream <robot@adios2>"
readonly subtree="thirdparty/nlohmann_json/json"
readonly repo="https://github.com/nlohmann/json.git"
readonly tag="master"
readonly shortlog="true"
readonly paths="
  CMakeLists.txt
  LICENSE.MIT
  nlohmann_json.natvis
  cmake
  include
  single_include
"

extract_source () {
    git_archive
}

. "${BASH_SOURCE%/*}/../update-common.sh"
