#!/bin/bash

# Run benchmarks while logging the intermediate results
# Publish findings in the progress tracker

set -e

DEV_VENV_PATH="/home/dev_user/dev_venv"

# shellcheck disable=SC1090,SC1091
if ! source "${DEV_VENV_PATH}/bin/activate"; then
    python3 -m venv "${DEV_VENV_PATH}"
    # shellcheck disable=SC1090,SC1091
    source "${DEV_VENV_PATH}/bin/activate"
    cd /src/ && make setup_env
fi

initial_log=logs/$(date -u --iso-8601=seconds).log

mkdir -p logs
poetry run python script/progress_tracker_utils/measure.py benchmarks > "$initial_log"

final_log=logs/$(date -u --iso-8601=seconds).log

cat -s "$initial_log" | sed '1d; $d' > "$final_log"
rm "$initial_log"

cp "$final_log" logs/latest.log

if [ -f .env ]
then
  # Set the last two environment variables in `.env` for the curl command below
  # (https://gist.github.com/mihow/9c7f559807069a03e302605691f85572)
  # shellcheck disable=SC2002,SC2046
  export $(cat .env | tail -n 2 | sed 's/#.*//g' | xargs  -d '\n')
fi

curl \
     -H 'Authorization: Bearer '"$PROGRESS_TRACKER_TOKEN"'' \
     -H 'Content-Type: application/json' \
     -d @.benchmarks/findings.json \
     -X POST "$PROGRESS_TRACKER_URL"/measurement
