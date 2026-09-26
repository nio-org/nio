#!/bin/sh
# Build the servers, run the load benchmark one target and scenario at a time,
# and build the HTML report. README.md has the options and the requirements.
set -e
cd "$(dirname "$0")"

NIO="${NIO:-../../nio}"
STAGES="${STAGES:-10,50,100,500,1000}"
DURATION="${DURATION:-5}"
SCENARIOS="${SCENARIOS:-plaintext json}"
TARGETS="${*:-node bun go java nio}"

if [ ! -x .venv/bin/python ]; then
    echo "== creating .venv (psutil)"
    python3 -m venv .venv
    .venv/bin/pip install --quiet psutil
fi

echo "== building servers"
mkdir -p bin results
"$NIO" build servers/server.nio -o bin/server-nio
go build -o bin/server-go servers/server.go
javac -d bin/java servers/Server.java

for t in $TARGETS; do
    for s in $SCENARIOS; do
        echo "== $t / $s"
        .venv/bin/python bench.py --target "$t" --scenario "$s" \
            --stages "$STAGES" --stage-duration "$DURATION"
    done
done

.venv/bin/python report.py
echo "open $(pwd)/results/report.html"
