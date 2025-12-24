#!/bin/bash

CERT_DIR="cert"
KEY_FILE="$CERT_DIR/server.key"
CRT_FILE="$CERT_DIR/server.crt"

echo "== SSL Certificate Generator =="

if ! command -v openssl &> /dev/null; then
    echo "[ERROR] OpenSSL not found. Please install it first."
    exit 1
fi

if [ ! -d "$CERT_DIR" ]; then
    mkdir "$CERT_DIR"
    echo "[INFO] Created directory: $CERT_DIR"
fi

if [ -f "$KEY_FILE" ]; then
    echo "[INFO] server.key already exists, skipping."
else
    echo "[INFO] Generating private key..."
    openssl genrsa -out "$KEY_FILE" 2048
fi

if [ -f "$CRT_FILE" ]; then
    echo "[INFO] server.crt already exists, skipping."
else
    echo "[INFO] Generating self-signed certificate..."
    openssl req -new -x509 \
        -key "$KEY_FILE" \
        -out "$CRT_FILE" \
        -days 365 \
        -subj "/C=TW/ST=Taiwan/L=Taipei/O=NTU/OU=EE/CN=localhost"
fi

echo "Certificate generation completed!"
echo "Key : $KEY_FILE"
echo "Cert: $CRT_FILE"
