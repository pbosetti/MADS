#!/bin/env bash

if [ $# -eq 0 ]; then
  echo "Usage:"
  echo "       ini     - echo an INI file template"
  echo "       compose - echo the compose file"
  exit 0
fi

case $1 in
  ini)
    cat /build/mads.ini
    ;;
  compose)
    cat /build/compose.yml
    ;;
  *)
    $1 "${@:2}"
    ;;
esac