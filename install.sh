#!/usr/bin/env bash
# ─── CedarHTTP install.sh ──────────────────────────────────────────────────
# Builds and installs CedarHTTP system-wide.
# Usage:
#   ./install.sh            # install to /usr/local
#   PREFIX=/opt ./install.sh
#   ./install.sh --uninstall

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
BINARY="cedar"
SERVICE_USER="www-cedar"
CONF_DIR="/etc/cedar"
LOG_DIR="/var/log/cedar"
DATA_DIR="/var/www/cedar"
SYSTEMD_DIR="/etc/systemd/system"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BOLD='\033[1m'; RESET='\033[0m'

info()  { echo -e "${GREEN}▶${RESET} $*"; }
warn()  { echo -e "${YELLOW}⚠${RESET}  $*"; }
error() { echo -e "${RED}✗${RESET} $*" >&2; exit 1; }
ok()    { echo -e "${GREEN}✓${RESET} $*"; }

# ── Uninstall ───────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--uninstall" ]]; then
    info "Uninstalling CedarHTTP..."
    systemctl stop cedar 2>/dev/null || true
    systemctl disable cedar 2>/dev/null || true
    rm -f "$SYSTEMD_DIR/cedar.service"
    systemctl daemon-reload 2>/dev/null || true
    rm -f "$PREFIX/bin/$BINARY"
    rm -rf "$CONF_DIR"
    ok "Uninstalled. Logs at $LOG_DIR and data at $DATA_DIR were kept."
    exit 0
fi

# ── Checks ──────────────────────────────────────────────────────────────────
[[ "$(uname)" == "Linux" ]] || error "CedarHTTP requires Linux"
command -v gcc  >/dev/null   || error "gcc not found"
command -v make >/dev/null   || error "make not found"

# ── Build ───────────────────────────────────────────────────────────────────
info "Building CedarHTTP (release)..."
make clean && make release
ok "Build complete: ./cedar ($(du -sh cedar | cut -f1))"

# ── Run tests ───────────────────────────────────────────────────────────────
info "Running test suite..."
if make test 2>/dev/null && ./cedar_tests; then
    ok "All tests passed"
else
    warn "Tests failed — installing anyway (check ./cedar_tests output)"
fi

# ── Create system user ───────────────────────────────────────────────────────
if ! id "$SERVICE_USER" &>/dev/null; then
    info "Creating service user: $SERVICE_USER"
    useradd -r -s /bin/false -d "$DATA_DIR" "$SERVICE_USER" 2>/dev/null || true
fi

# ── Create directories ───────────────────────────────────────────────────────
info "Creating directories..."
install -d -m 755 "$PREFIX/bin"
install -d -m 750 -o "$SERVICE_USER" -g "$SERVICE_USER" "$LOG_DIR"   2>/dev/null || install -d -m 750 "$LOG_DIR"
install -d -m 755 "$CONF_DIR"
install -d -m 755 "$DATA_DIR"

# ── Install binary ───────────────────────────────────────────────────────────
info "Installing binary to $PREFIX/bin/$BINARY..."
install -m 755 cedar "$PREFIX/bin/$BINARY"
ok "Binary installed"

# ── Install config ───────────────────────────────────────────────────────────
if [[ ! -f "$CONF_DIR/server.json" ]]; then
    info "Installing default config to $CONF_DIR/server.json..."
    sed "s|\"./static\"|\"$DATA_DIR\"|g; \
         s|\"./logs/access.log\"|\"$LOG_DIR/access.log\"|g; \
         s|\"./logs/error.log\"|\"$LOG_DIR/error.log\"|g; \
         s|\"./cedar.pid\"|\"$DATA_DIR/cedar.pid\"|g" \
        config/server.json > "$CONF_DIR/server.json"
    ok "Config installed"
else
    warn "Config already exists at $CONF_DIR/server.json — not overwritten"
fi

# ── Install sample static files ───────────────────────────────────────────────
if [[ ! -f "$DATA_DIR/index.html" ]]; then
    info "Installing sample web root to $DATA_DIR..."
    cp -r static/. "$DATA_DIR/"
    ok "Web root installed"
fi

# ── systemd service ───────────────────────────────────────────────────────────
if command -v systemctl &>/dev/null; then
    info "Installing systemd service..."
    cat > "$SYSTEMD_DIR/cedar.service" << UNIT
[Unit]
Description=CedarHTTP Web Server
After=network.target
Documentation=https://github.com/yourname/cedarhttp

[Service]
Type=simple
User=$SERVICE_USER
ExecStart=$PREFIX/bin/$BINARY $CONF_DIR/server.json
ExecReload=/bin/kill -HUP \$MAINPID
Restart=on-failure
RestartSec=5s
LimitNOFILE=65536
# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths=$LOG_DIR $DATA_DIR

[Install]
WantedBy=multi-user.target
UNIT

    systemctl daemon-reload
    ok "systemd service installed: cedar.service"
    echo ""
    echo -e "  ${BOLD}Start:${RESET}   systemctl start cedar"
    echo -e "  ${BOLD}Enable:${RESET}  systemctl enable cedar"
    echo -e "  ${BOLD}Logs:${RESET}    journalctl -u cedar -f"
fi

echo ""
echo -e "${GREEN}${BOLD}✓ CedarHTTP installed successfully${RESET}"
echo -e "  Binary:  $PREFIX/bin/$BINARY"
echo -e "  Config:  $CONF_DIR/server.json"
echo -e "  Webroot: $DATA_DIR"
echo -e "  Logs:    $LOG_DIR"
echo ""
