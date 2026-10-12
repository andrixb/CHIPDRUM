#!/bin/sh
# Start the CHIPGENE firmware hub and open it in your browser.
cd "$(dirname "$0")/.."
exec python3 web/serve.py "$@"
