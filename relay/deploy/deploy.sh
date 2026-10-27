#!/bin/sh
# deploy.sh: put the hako relay on its box (RELAY-SERVER.md). You run this from
# your desk with your key; it builds on the box, so nothing binary is copied.
#   ./deploy.sh              build, install, (re)start, check
#   ./deploy.sh invite [n]   print n single-use invite links
#   ./deploy.sh status       the service and its last log lines
#   ./deploy.sh github       turn on GitHub sign-in (google: Google): asks for the app's id
#                            and secret, which go to the box over ssh stdin only
# KEY=path/to/key.pem and HOST=user@host override the defaults.
set -eu
KEY=${KEY:-$HOME/.ssh/mithraeum-relay.pem}
HOST=${HOST:-admin@relay.mithraeum.studio}
here=$(cd "$(dirname "$0")/.." && pwd)
[ -r "$KEY" ] || { echo "no key at $KEY (KEY=path ./deploy.sh)"; exit 1; }
on_box() { ssh -i "$KEY" -o IdentitiesOnly=yes "$HOST" "$@"; }

case "${1:-deploy}" in
invite) on_box "sudo -u hako-relay /usr/local/bin/hako-relay invite ${2:-1} -d /var/lib/hako-relay" \
	| sed 's|^|https://hako.mithraeum.studio/#invite=|'; exit ;;
status) on_box "systemctl --no-pager status hako-relay | head -6; sudo journalctl -u hako-relay -n 20 --no-pager"; exit ;;
github|google)
	printf '%s client id: ' "$1"; read -r id
	stty -echo; printf 'client secret (not shown): '; read -r sec; stty echo; echo
	printf '%s %s\n' "$id" "$sec" | on_box "sudo -u hako-relay sh -c \"umask 077; cat > /var/lib/hako-relay/$1\" && sudo systemctl restart hako-relay"
	sleep 1; on_box 'curl -s http://127.0.0.1:8790/v1/health'; exit ;;
deploy) ;;
*) echo "usage: $0 [deploy | invite [n] | status | github | google]"; exit 2 ;;
esac

tar -C "$here" -czf - relay.c Makefile deploy/hako-relay.service deploy/Caddyfile \
	| on_box 'rm -rf /tmp/hako-relay && mkdir -p /tmp/hako-relay && tar -C /tmp/hako-relay -xzf -'
on_box 'set -e
cd /tmp/hako-relay
if ! command -v cc >/dev/null || [ ! -e /usr/include/openssl/evp.h ]; then
	sudo apt-get update -q && sudo apt-get install -y -q build-essential libssl-dev
fi
command -v curl >/dev/null || sudo apt-get install -y -q curl     # GitHub and Google sign-in ask through it
make relay OPENSSL=
id hako-relay >/dev/null 2>&1 || sudo useradd --system --no-create-home --shell /usr/sbin/nologin hako-relay
sudo install -m 755 relay /usr/local/bin/hako-relay
sudo install -m 644 deploy/hako-relay.service /etc/systemd/system/hako-relay.service
sudo install -m 644 deploy/Caddyfile /etc/caddy/Caddyfile
sudo systemctl daemon-reload
sudo systemctl enable hako-relay >/dev/null 2>&1
sudo systemctl restart hako-relay
sudo systemctl reload caddy
cd / && rm -rf /tmp/hako-relay'
sleep 1
curl -fsS https://relay.mithraeum.studio/v1/health >/dev/null && echo "relay: live at https://relay.mithraeum.studio" \
	|| { echo "relay: not answering yet, see ./deploy.sh status"; exit 1; }
