#!/bin/sh
# Runs the hg_pc multiplayer server on port 7777. Its log, bans and player passwords are kept next to
# this script. Extra options go on the end, e.g.:  ./hg_server.sh --password word --admin word
#   --password word   players need this to join        --admin word   for /login, then /kick /ban /passwords...
#   --chat map        chat reaches the same map only    --max N        player limit (up to 16)
cd "$(dirname "$0")" || exit 1
exec ./hg_server --port 7777 --log hg_server.log --bans hg_server_bans.txt --accounts hg_server_accounts.txt "$@"
