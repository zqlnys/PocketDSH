#!/bin/sh
# Verifies that the test counts written in the documentation match what the suites
# actually produce. Documentation drifting from reality is not hypothetical here - three
# consecutive rounds found it, in three different documents.
set -u
python3 "$(dirname "$0")/check_docs_impl.py"
