#!/bin/bash

git clang-format -f --style=google "$(git merge-base master HEAD)"
