#!/bin/bash
# Checks the broker's TLS certificate: trust chain, whether it lists the IP the
# board connects to, and days until it expires.
# Usage: check-broker-cert.sh [host] [port] [ca-file]
host="${1:-${MQTT_HOST:-127.0.0.1}}"
port="${2:-8883}"
ca="${3:-tools/simulator/ca.crt}"
[ -r "$ca" ] || { echo "CA file not readable: $ca"; exit 2; }

tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
if [[ "$host" =~ ^[0-9.]+$ ]]; then verify=(-verify_ip "$host"); else verify=(-verify_hostname "$host"); fi
echo | openssl s_client -connect "$host:$port" -CAfile "$ca" "${verify[@]}" >"$tmp" 2>&1 || true

grep -m1 "Verify return code" "$tmp" || echo "No TLS answer from $host:$port"
openssl x509 -in "$tmp" -noout -subject -ext subjectAltName 2>/dev/null
end=$(openssl x509 -in "$tmp" -noout -enddate 2>/dev/null | cut -d= -f2)
if [ -n "$end" ]; then
    days=$(( ( $(date -d "$end" +%s) - $(date +%s) ) / 86400 ))
    echo "Expires: $end ($days days left)"
fi
