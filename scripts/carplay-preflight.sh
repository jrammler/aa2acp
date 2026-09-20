#!/usr/bin/env bash
# Start and monitor a management-UI CarPlay preflight.
set -euo pipefail

fresh=0
case ${1:-} in
  --help)
    cat <<'EOF'
Usage: scripts/carplay-preflight.sh [--fresh]

Starts CarPlay preflight, prints relevant daemon logs when AA2ACP_PI_HOST is
set, confirms the Pi-side Bluetooth numeric-comparison prompt, and exits when
preflight succeeds or fails.

--fresh clears local Bluetooth, AirPlay pairing, and cached capabilities first.

Environment:
  AA2ACP_UI_URL                 Management UI URL (default:
                                 http://aa2acp-pi.local:8080)
  AA2ACP_PI_HOST                Optional SSH destination for live daemon logs
  AA2ACP_PREFLIGHT_TIMEOUT_SECONDS  Terminal-state timeout (default: 180)
EOF
    exit 0
    ;;
  --fresh)
    fresh=1
    shift
    ;;
  "")
    ;;
  *)
    echo "Usage: $0 [--fresh]" >&2
    exit 2
    ;;
esac
[[ $# -eq 0 ]] || { echo "Usage: $0 [--fresh]" >&2; exit 2; }

base_url=${AA2ACP_UI_URL:-http://aa2acp-pi.local:8080}
timeout_seconds=${AA2ACP_PREFLIGHT_TIMEOUT_SECONDS:-180}
pi_host=${AA2ACP_PI_HOST:-}

fetch_page() {
  curl -fsS --max-time 3 "$base_url/"
}

csrf_token() {
  grep -o 'name=csrf value="[^"]*"' | head -1 | cut -d'"' -f2
}

post_form() {
  local endpoint=$1
  local page=$2
  shift 2
  local csrf
  csrf=$(printf '%s' "$page" | csrf_token)
  [[ -n $csrf ]] || { echo "Could not obtain management UI CSRF token" >&2; return 1; }
  curl -fsS --max-time 8 -X POST --data "csrf=$csrf" "$@" \
    "$base_url/$endpoint" >/dev/null
}

page=""
for ((attempt = 0; attempt < 20; ++attempt)); do
  page=$(fetch_page || true)
  [[ -n $page ]] && break
  sleep 1
done
[[ -n $page ]] || { echo "Management UI did not become ready" >&2; exit 1; }

if [[ $fresh == 1 ]]; then
  echo "Clearing local head-unit state..."
  post_form head-unit-forget "$page"
  page=$(fetch_page)
fi

log_pid=""
if [[ -n $pi_host ]]; then
  # Killing this SSH process closes the remote journal follower instead of
  # waiting for journalctl to emit a further line after terminal state.
  ssh "$pi_host" \
    'sudo -n journalctl -u aa2acp -n 0 -f -o short-iso | grep --line-buffered -E "Bluetooth:|iAP2: link established|CSM: identification|Wi-Fi: joining|AirPlay: (Pair-Setup|Pair-Verify|screen SETUP)|Management: CarPlay preflight"' &
  log_pid=$!
fi
cleanup() {
  [[ -z $log_pid ]] || kill "$log_pid" 2>/dev/null || true
  [[ -z $log_pid ]] || wait "$log_pid" 2>/dev/null || true
}
trap cleanup EXIT

post_form carplay-prepare "$page"
echo "Preflight started."

saw_active=0
pairing_confirmed=0
for ((second = 0; second < timeout_seconds; ++second)); do
  page=$(fetch_page || true)
  if [[ -z $page ]]; then
    sleep 1
    continue
  fi

  active=$(printf '%s' "$page" |
    sed -n 's/.*data-preflight-active="\([01]\)".*/\1/p' | head -1)
  [[ $active == "1" ]] && saw_active=1

  if [[ $pairing_confirmed == 0 ]] &&
      printf '%s' "$page" | grep -q 'action="/bluetooth-confirm"'; then
    request_id=$(printf '%s' "$page" |
      sed -n 's/.*name="id" value="\([0-9][0-9]*\)".*/\1/p' | head -1)
    code=$(printf '%s' "$page" |
      grep -o '<b>[0-9]\{6\}</b>' | head -1 | tr -cd '0-9')
    [[ -n $request_id && -n $code ]] || {
      echo "Could not parse Bluetooth pairing confirmation" >&2
      exit 1
    }
    echo "Bluetooth code: $code"
    post_form bluetooth-confirm "$page" \
      --data "id=$request_id&decision=confirm"
    pairing_confirmed=1
    echo "Pi-side confirmation submitted; confirm the same code on the head unit."
  fi

  if [[ $saw_active == 1 && $active == "0" ]]; then
    detail=$(printf '%s' "$page" |
      sed -n 's/.*CarPlay preparation: \([^<]*\)<\/p>.*/\1/p' | head -1)
    echo "Preflight finished: ${detail:-terminal state reached}"
    [[ $detail != failed* ]]
    exit
  fi
  sleep 1
done

echo "Preflight did not reach a terminal state within ${timeout_seconds}s" >&2
exit 1
