#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG_NAME="ec-systemcore"
PKG_VERSION="${PKG_VERSION:-1.0.0}"
PKG_ARCH="arm64"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/ipk_stage}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
DAEMON_BIN="${DAEMON_BIN:-${ROOT_DIR}/build/daemon/ec-systemcore-daemon}"
DASHBOARD_DIR="${ROOT_DIR}/dashboard"
DASHBOARD_OUT="${DASHBOARD_DIR}/dist"

if ! command -v opkg-build >/dev/null 2>&1; then
  echo "error: opkg-build is required. Install opkg-utils." >&2
  exit 1
fi

if [[ ! -f "${DAEMON_BIN}" ]]; then
  found_bin="$(find "${ROOT_DIR}/build" -type f -name ec-systemcore-daemon -print -quit 2>/dev/null || true)"
  if [[ -n "${found_bin}" ]]; then
    DAEMON_BIN="${found_bin}"
  else
    echo "error: ec-systemcore-daemon binary not found. Set DAEMON_BIN or build the daemon first." >&2
    exit 1
  fi
fi

rm -rf "${STAGE_DIR}"
mkdir -p \
  "${STAGE_DIR}/CONTROL" \
  "${STAGE_DIR}/usr/bin" \
  "${STAGE_DIR}/etc/systemd/system" \
  "${STAGE_DIR}/var/www/html/apps/ec-dashboard" \
  "${DIST_DIR}"

install -m 0755 "${DAEMON_BIN}" "${STAGE_DIR}/usr/bin/ec-systemcore-daemon"
install -m 0644 "${ROOT_DIR}/daemon/ec-systemcore.service" \
  "${STAGE_DIR}/etc/systemd/system/ec-systemcore.service"

if command -v bun >/dev/null 2>&1; then
  pushd "${DASHBOARD_DIR}" >/dev/null
  if [[ -f bun.lockb || -f bun.lock ]]; then
    bun install --frozen-lockfile
  else
    bun install
  fi
  rm -rf "${DASHBOARD_OUT}"
  mkdir -p "${DASHBOARD_OUT}"
  bun build server.ts --target=bun --outfile "${DASHBOARD_OUT}/server.js"
  cp -R public "${DASHBOARD_OUT}/public"
  cp package.json tsconfig.json "${DASHBOARD_OUT}/"
  popd >/dev/null
  cp -R "${DASHBOARD_OUT}/." "${STAGE_DIR}/var/www/html/apps/ec-dashboard/"
else
  echo "warning: bun not found; packaging dashboard source/assets without bundling." >&2
  cp "${DASHBOARD_DIR}/package.json" "${DASHBOARD_DIR}/server.ts" "${DASHBOARD_DIR}/tsconfig.json" \
    "${STAGE_DIR}/var/www/html/apps/ec-dashboard/"
  cp -R "${DASHBOARD_DIR}/public" "${STAGE_DIR}/var/www/html/apps/ec-dashboard/public"
fi

cat >"${STAGE_DIR}/CONTROL/control" <<CONTROL
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Architecture: ${PKG_ARCH}
Maintainer: ec-systemcore maintainers
Depends: libsoem
Section: net
Priority: optional
Description: ec-systemcore EtherCAT MainDevice daemon and dashboard for Raspberry Pi CM5.
CONTROL

cat >"${STAGE_DIR}/CONTROL/postinst" <<'POSTINST'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload
  systemctl enable --now ec-systemcore
fi
exit 0
POSTINST

cat >"${STAGE_DIR}/CONTROL/prerm" <<'PRERM'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
  systemctl stop ec-systemcore || true
  systemctl disable ec-systemcore || true
fi
exit 0
PRERM

chmod 0755 "${STAGE_DIR}/CONTROL/postinst" "${STAGE_DIR}/CONTROL/prerm"
opkg-build "${STAGE_DIR}" "${DIST_DIR}"
