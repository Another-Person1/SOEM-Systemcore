#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG_NAME="ec-systemcore"
PKG_VERSION="${PKG_VERSION:-1.0.0}"
readonly PKG_ARCH="arm64"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/ipk_stage}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
DASHBOARD_DIR="${ROOT_DIR}/dashboard"
DASHBOARD_OUT="${DASHBOARD_DIR}/dist"
CONTROL_DIR="${ROOT_DIR}/CONTROL"

if ! command -v opkg-build >/dev/null 2>&1; then
  echo "error: opkg-build is required. Install opkg-utils." >&2
  exit 1
fi

if [[ -z "${DAEMON_BIN:-}" ]]; then
  for candidate in \
    "${ROOT_DIR}/build/daemon/ec-systemcore-daemon" \
    "${ROOT_DIR}/build/daemon/daemon/ec-systemcore-daemon"; do
    if [[ -f "${candidate}" ]]; then
      DAEMON_BIN="${candidate}"
      break
    fi
  done
fi

if [[ -z "${DAEMON_BIN:-}" || ! -f "${DAEMON_BIN}" ]]; then
  found_bin="$(find "${ROOT_DIR}/build" -type f -name ec-systemcore-daemon -print -quit 2>/dev/null || true)"
  if [[ -n "${found_bin}" ]]; then
    DAEMON_BIN="${found_bin}"
  else
    echo "error: ec-systemcore-daemon binary not found. Set DAEMON_BIN or build the daemon first." >&2
    exit 1
  fi
fi

if command -v aarch64-linux-gnu-readelf >/dev/null 2>&1; then
  binary_machine="$(aarch64-linux-gnu-readelf -h "${DAEMON_BIN}" | awk -F: '/Machine:/ { gsub(/^[ \t]+/, "", $2); print $2 }')"
  if [[ "${binary_machine}" != "AArch64" ]]; then
    echo "error: ${DAEMON_BIN} is '${binary_machine}', expected AArch64 for arm64 IPK." >&2
    exit 1
  fi
elif command -v file >/dev/null 2>&1; then
  binary_file_type="$(file -b "${DAEMON_BIN}")"
  if [[ "${binary_file_type}" != *"aarch64"* && "${binary_file_type}" != *"ARM aarch64"* && "${binary_file_type}" != *"ARM64"* ]]; then
    echo "error: ${DAEMON_BIN} is '${binary_file_type}', expected AArch64 for arm64 IPK." >&2
    exit 1
  fi
else
  echo "warning: unable to verify daemon binary architecture; readelf or file not found." >&2
fi

rm -rf "${STAGE_DIR}"
mkdir -p \
  "${STAGE_DIR}/CONTROL" \
  "${STAGE_DIR}/usr/bin" \
  "${STAGE_DIR}/etc/systemd/system" \
  "${STAGE_DIR}/var/www/html/apps/ec-dashboard" \
  "${DIST_DIR}"

install -m 0755 "${DAEMON_BIN}" "${STAGE_DIR}/usr/bin/ec-systemcore-daemon"
chmod 0755 "${STAGE_DIR}/usr/bin/ec-systemcore-daemon"
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

install -m 0644 "${CONTROL_DIR}/control" "${STAGE_DIR}/CONTROL/control"
sed -i \
  -e "s/^Package:.*/Package: ${PKG_NAME}/" \
  -e "s/^Version:.*/Version: ${PKG_VERSION}/" \
  -e "s/^Architecture:.*/Architecture: ${PKG_ARCH}/" \
  "${STAGE_DIR}/CONTROL/control"

if ! grep -qx "Architecture: ${PKG_ARCH}" "${STAGE_DIR}/CONTROL/control"; then
  echo "error: CONTROL/control must declare Architecture: ${PKG_ARCH}" >&2
  exit 1
fi
if ! grep -Eq '^Depends: .*systemd' "${STAGE_DIR}/CONTROL/control" ||
   ! grep -Eq '^Depends: .*bun' "${STAGE_DIR}/CONTROL/control"; then
  echo "error: CONTROL/control must depend on systemd and bun for the SystemCore runtime environment." >&2
  exit 1
fi

install -m 0755 "${CONTROL_DIR}/postinst" "${STAGE_DIR}/CONTROL/postinst"
install -m 0755 "${CONTROL_DIR}/prerm" "${STAGE_DIR}/CONTROL/prerm"

chmod 0755 "${STAGE_DIR}/CONTROL/postinst" "${STAGE_DIR}/CONTROL/prerm"
opkg-build "${STAGE_DIR}" "${DIST_DIR}"
