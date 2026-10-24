#!/bin/bash
# Re-signs the Mosquitto server certificate when this PC's IP addresses change.
# Installed as a NetworkManager dispatcher script ($1 = interface, $2 = action).
# Manual run: sudo ./resign-broker-cert.sh --force
set -u
CERT_DIR=/etc/mosquitto/certs
TAG=iotcommander-cert

if [ "${1:-}" != "--force" ]; then
    case "${2:-}" in
        up|dhcp4-change) ;;
        *) exit 0 ;;
    esac
fi

exec 9>/run/iotcommander-cert.lock
flock -n 9 || exit 0

log() { logger -t "$TAG" "$*"; echo "$*"; }

# Give DHCP a moment to finish so the new address is visible.
[ "${1:-}" = "--force" ] || sleep 2

mapfile -t ips < <(ip -4 -o addr show scope global \
    | awk '$2 !~ /^(docker|br-|veth|virbr|lo)/ {split($4, a, "/"); print a[1]}' | sort -u)
[ "${#ips[@]}" -gt 0 ] || exit 0

current=$(openssl x509 -in "$CERT_DIR/server.crt" -noout -ext subjectAltName 2>/dev/null \
    | grep -o 'IP Address:[0-9.]*' | cut -d: -f2)
missing=0
for ip in "${ips[@]}"; do
    printf '%s\n' "$current" | grep -qx "$ip" || missing=1
done
if [ "$missing" -eq 0 ] && [ "${1:-}" != "--force" ]; then
    exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

san="IP:127.0.0.1"
for ip in "${ips[@]}"; do san="$san,IP:$ip"; done
cat > "$work/ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=$san
EOF

if ! openssl x509 -req -in "$CERT_DIR/server.csr" -CA "$CERT_DIR/ca.crt" \
        -CAkey "$CERT_DIR/ca.key" -CAserial "$CERT_DIR/ca.srl" \
        -out "$work/server.crt" -days 825 -sha256 -extfile "$work/ext" 2>/dev/null; then
    log "signing failed; certificate unchanged"; exit 1
fi
if ! openssl verify -CAfile "$CERT_DIR/ca.crt" "$work/server.crt" >/dev/null 2>&1; then
    log "new certificate did not verify; certificate unchanged"; exit 1
fi
new_key=$(openssl x509 -in "$work/server.crt" -noout -pubkey | sha256sum)
old_key=$(openssl pkey -in "$CERT_DIR/server.key" -pubout | sha256sum)
if [ "$new_key" != "$old_key" ]; then
    log "certificate does not match server.key; unchanged"; exit 1
fi

cp -p "$CERT_DIR/server.crt" "$CERT_DIR/server.crt.bak"
install -o mosquitto -g mosquitto -m 644 "$work/server.crt" "$CERT_DIR/server.crt"
if ! systemctl restart mosquitto || ! systemctl is-active --quiet mosquitto; then
    cp -p "$CERT_DIR/server.crt.bak" "$CERT_DIR/server.crt"
    systemctl restart mosquitto
    log "mosquitto failed with the new certificate; restored the old one"
    exit 1
fi
log "re-signed broker certificate for: ${ips[*]}"
