# Runs on the Mac. Creates a permanent self-signed code-signing identity "Mr PinPin Code Signing" in its own
# keychain, so every PinPin Host build has the same designated requirement and keeps its TCC grants.
set -e
D=~/.local/share/pinpin-host/signing; mkdir -p "$D"; chmod 700 "$D"; cd "$D"
KC=~/Library/Keychains/pinpin-signing.keychain-db
if [ -f "$KC" ] && security find-certificate -c "Mr PinPin Code Signing" "$KC" >/dev/null 2>&1; then echo "identity exists"; exit 0; fi
[ -f keychain-pass ] || (LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 32 > keychain-pass; chmod 600 keychain-pass)
P=$(cat keychain-pass)
cat > cert.cnf <<'CNF'
[req]
distinguished_name=dn
x509_extensions=ext
prompt=no
[dn]
CN=Mr PinPin Code Signing
O=Mr PinPin
[ext]
basicConstraints=critical,CA:false
keyUsage=critical,digitalSignature
extendedKeyUsage=critical,codeSigning
CNF
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 7300 -config cert.cnf 2>/dev/null
/usr/bin/openssl pkcs12 -export -inkey key.pem -in cert.pem -out identity.p12 -passout pass:"$P" -name "Mr PinPin Code Signing"
chmod 600 key.pem identity.p12
security create-keychain -p "$P" "$KC" 2>/dev/null || true
security set-keychain-settings "$KC"            # no auto-lock
security unlock-keychain -p "$P" "$KC"
security import identity.p12 -k "$KC" -P "$P" -T /usr/bin/codesign >/dev/null
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$P" "$KC" >/dev/null
security list-keychains -d user -s $(security list-keychains -d user | tr -d '"') "$KC"
security find-identity -p codesigning "$KC"
