import { mkdir, readdir, readFile, stat, statfs, unlink, writeFile } from "node:fs/promises";
import { basename, extname, join, resolve } from "node:path";

const DASHBOARD_VERSION = "2026.0.1";
const DAEMON_VERSION = "2026.1.0";
const DEFAULT_PORT = 80;
const PORT = Number(Bun.env.PORT ?? DEFAULT_PORT);
const PUBLIC_DIR = join(import.meta.dir, "public");
const CONFIG_PATH = Bun.env.EC_DASHBOARD_CONFIG ?? "/etc/ethercat/ec-configuration.json";
const SOCKET_PATH = Bun.env.EC_DASHBOARD_SOCKET ?? "/var/run/ethercat_maindevice.sock";
const RESTRICTED_INTERFACES = new Set(["eth0", "wlan0", "usb0", "lo"]);

type InterfaceMapping = {
  logical_name: string;
  physical_interface: string;
};

type DashboardConfig = {
  allow_restricted_interfaces: boolean;
  log_directory: string;
  log_count_limit: number;
  free_space_threshold_mb: number;
  interface_mappings: InterfaceMapping[];
};

type AdapterInfo = {
  name: string;
  logicalName: string;
  mac: string;
  restricted: boolean;
  locked: boolean;
  rxBytesPerSecond: number;
  txBytesPerSecond: number;
};

type MainDeviceStatus = {
  daemonStatus: number;
  mainDeviceState: number;
  activeAdapters: number;
  subDeviceCount: number;
  activeFaults: number;
  mainDeviceJitterUs: number;
  lostFrames: number;
  interfaceName: string;
  logicalName: string;
  updatedAt: number;
};

type ConfigState = {
  config: DashboardConfig;
  isConfigLoaded: boolean;
  regeneratedFailsafe: boolean;
  configError: string | null;
};

const defaultConfig: DashboardConfig = {
  allow_restricted_interfaces: false,
  log_directory: "/var/log/ethercat",
  log_count_limit: 10,
  free_space_threshold_mb: 50,
  interface_mappings: [
    {
      logical_name: "EC_Trunk",
      physical_interface: "eth1"
    }
  ]
};

let configState: ConfigState = {
  config: defaultConfig,
  isConfigLoaded: true,
  regeneratedFailsafe: false,
  configError: null
};

let latestMainDeviceStatus: MainDeviceStatus = offlineStatus();
let activeLogPath: string | null = null;
let activeLogOffset = 0;
const logSockets = new Set<ServerWebSocket>();
const adapterSamples = new Map<string, { rx: number; tx: number; at: number }>();

type ServerWebSocket = Parameters<NonNullable<ServeOptions["websocket"]>["open"]>[0];
type ServeOptions = Parameters<typeof Bun.serve>[0];

function offlineStatus(): MainDeviceStatus {
  return {
    daemonStatus: 0,
    mainDeviceState: 0,
    activeAdapters: 0,
    subDeviceCount: 0,
    activeFaults: 0,
    mainDeviceJitterUs: 0,
    lostFrames: 0,
    interfaceName: "",
    logicalName: "",
    updatedAt: Date.now()
  };
}

function json(data: unknown, init: ResponseInit = {}) {
  return new Response(JSON.stringify(data), {
    ...init,
    headers: {
      "content-type": "application/json; charset=utf-8",
      ...(init.headers ?? {})
    }
  });
}

function text(value: string, status = 200) {
  return new Response(value, {
    status,
    headers: { "content-type": "text/plain; charset=utf-8" }
  });
}

function normalizeConfig(value: Partial<DashboardConfig>): DashboardConfig {
  const maxLogs = Number(value.log_count_limit);
  const freeMb = Number(value.free_space_threshold_mb);
  const mappings = Array.isArray(value.interface_mappings) ? value.interface_mappings : [];
  return {
    allow_restricted_interfaces: Boolean(value.allow_restricted_interfaces),
    log_directory: typeof value.log_directory === "string" && value.log_directory.trim()
      ? value.log_directory.trim()
      : defaultConfig.log_directory,
    log_count_limit: [10, 20, 50, 100].includes(maxLogs) ? maxLogs : defaultConfig.log_count_limit,
    free_space_threshold_mb: Number.isFinite(freeMb) && freeMb >= 1 ? Math.floor(freeMb) : defaultConfig.free_space_threshold_mb,
    interface_mappings: mappings
      .filter((item) => item && typeof item.logical_name === "string" && typeof item.physical_interface === "string")
      .map((item) => ({
        logical_name: item.logical_name.trim() || "EC_Trunk",
        physical_interface: item.physical_interface.trim() || "eth1"
      }))
  };
}

async function loadConfig(): Promise<void> {
  try {
    const raw = await Bun.file(CONFIG_PATH).text();
    configState = {
      config: normalizeConfig(JSON.parse(raw)),
      isConfigLoaded: true,
      regeneratedFailsafe: false,
      configError: null
    };
  } catch (error) {
    configState = {
      config: defaultConfig,
      isConfigLoaded: false,
      regeneratedFailsafe: true,
      configError: error instanceof Error ? error.message : "Unable to read configuration"
    };
    await persistConfig(defaultConfig, true);
  }
}

async function persistConfig(config: DashboardConfig, keepFailsafe: boolean): Promise<void> {
  try {
    await mkdir(resolve(CONFIG_PATH, ".."), { recursive: true });
    await Bun.write(CONFIG_PATH, JSON.stringify(config, null, 2) + "\n");
    configState = {
      config,
      isConfigLoaded: !keepFailsafe,
      regeneratedFailsafe: keepFailsafe,
      configError: keepFailsafe ? configState.configError : null
    };
  } catch (error) {
    configState = {
      config,
      isConfigLoaded: false,
      regeneratedFailsafe: true,
      configError: error instanceof Error ? error.message : "Unable to write configuration"
    };
  }
}

async function listAdapters(): Promise<AdapterInfo[]> {
  const netDir = "/sys/class/net";
  let names: string[] = [];
  try {
    names = await readdir(netDir);
  } catch {
    names = ["eth1"];
  }

  const mappings = new Map(configState.config.interface_mappings.map((item) => [item.physical_interface, item.logical_name]));
  const adapters = await Promise.all(names.map(async (name) => {
    const restricted = RESTRICTED_INTERFACES.has(name);
    if (restricted && !configState.config.allow_restricted_interfaces) return null;
    const [mac, rxBytes, txBytes] = await Promise.all([
      readTextOr(`${netDir}/${name}/address`, "00:00:00:00:00:00"),
      readNumberOr(`${netDir}/${name}/statistics/rx_bytes`, 0),
      readNumberOr(`${netDir}/${name}/statistics/tx_bytes`, 0)
    ]);
    const now = Date.now();
    const previous = adapterSamples.get(name);
    const elapsedSeconds = previous ? Math.max((now - previous.at) / 1000, 0.001) : 1;
    adapterSamples.set(name, { rx: rxBytes, tx: txBytes, at: now });
    return {
      name,
      logicalName: mappings.get(name) ?? name,
      mac: mac.trim(),
      restricted,
      locked: mappings.has(name),
      rxBytesPerSecond: previous ? Math.max(0, Math.round((rxBytes - previous.rx) / elapsedSeconds)) : 0,
      txBytesPerSecond: previous ? Math.max(0, Math.round((txBytes - previous.tx) / elapsedSeconds)) : 0
    };
  }));
  return adapters.filter((item): item is AdapterInfo => Boolean(item));
}

async function readTextOr(path: string, fallback: string): Promise<string> {
  try {
    return await Bun.file(path).text();
  } catch {
    return fallback;
  }
}

async function readNumberOr(path: string, fallback: number): Promise<number> {
  const raw = await readTextOr(path, String(fallback));
  const value = Number(raw.trim());
  return Number.isFinite(value) ? value : fallback;
}

async function listLogs() {
  await mkdir(configState.config.log_directory, { recursive: true });
  const entries = await readdir(configState.config.log_directory);
  const logs = await Promise.all(entries
    .filter((name) => extname(name) === ".wpilog")
    .map(async (name) => {
      const path = join(configState.config.log_directory, name);
      const info = await stat(path);
      return {
        name,
        size: info.size,
        modifiedAt: info.mtimeMs
      };
    }));
  return logs.sort((a, b) => b.modifiedAt - a.modifiedAt);
}

async function rotateLogs(): Promise<void> {
  try {
    await mkdir(configState.config.log_directory, { recursive: true });
    const logs = await listLogs();
    const fsInfo = await statfs(configState.config.log_directory);
    const freeMb = Math.floor(Number(fsInfo.bavail * fsInfo.bsize) / 1024 / 1024);
    const sortedOldest = [...logs].sort((a, b) => a.modifiedAt - b.modifiedAt);

    while (
      sortedOldest.length > configState.config.log_count_limit ||
      (freeMb < configState.config.free_space_threshold_mb && sortedOldest.length > 0)
    ) {
      const next = sortedOldest.shift();
      if (!next) break;
      await unlink(join(configState.config.log_directory, next.name));
    }
  } catch (error) {
    console.error("Log rotation warning:", error);
  }
}

function safeLogPath(name: string): string | null {
  const clean = basename(name);
  if (clean !== name || extname(clean) !== ".wpilog") return null;
  return join(configState.config.log_directory, clean);
}

async function deleteLogs(names: string[]): Promise<number> {
  let deleted = 0;
  for (const name of names) {
    const path = safeLogPath(name);
    if (!path) continue;
    try {
      await unlink(path);
      deleted += 1;
    } catch {
      // File may already be gone.
    }
  }
  await rotateLogs();
  return deleted;
}

async function deleteAllLogs(): Promise<number> {
  const logs = await listLogs();
  return deleteLogs(logs.map((item) => item.name));
}

function parseCString(bytes: Uint8Array): string {
  const end = bytes.indexOf(0);
  return new TextDecoder().decode(end >= 0 ? bytes.slice(0, end) : bytes).trim();
}

function parseMainDeviceStatus(packet: Uint8Array): MainDeviceStatus | null {
  if (packet.byteLength < 128) return null;
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  return {
    daemonStatus: view.getUint8(0),
    mainDeviceState: view.getUint8(1),
    activeAdapters: view.getUint8(2),
    subDeviceCount: view.getUint8(3),
    activeFaults: view.getUint16(4, true),
    mainDeviceJitterUs: view.getUint16(6, true),
    lostFrames: view.getUint32(8, true),
    interfaceName: parseCString(packet.slice(12, 28)),
    logicalName: parseCString(packet.slice(28, 44)),
    updatedAt: Date.now()
  };
}

async function connectStatusSocket() {
  try {
    await Bun.connect({
      unix: SOCKET_PATH,
      socket: {
        data(_socket, data) {
          const status = parseMainDeviceStatus(new Uint8Array(data));
          if (status) latestMainDeviceStatus = status;
        },
        close() {
          latestMainDeviceStatus = offlineStatus();
          setTimeout(() => void connectStatusSocket(), 1000);
        },
        error() {
          latestMainDeviceStatus = offlineStatus();
          setTimeout(() => void connectStatusSocket(), 1000);
        }
      }
    });
  } catch {
    latestMainDeviceStatus = offlineStatus();
    setTimeout(() => void connectStatusSocket(), 1000);
  }
}

async function sendMainDeviceCommand(commandType: number, targetSubDevice = 0, targetPort = 0, payload = new Uint8Array()): Promise<void> {
  const packet = new Uint8Array(64);
  packet[0] = commandType & 0xff;
  packet[1] = targetSubDevice & 0xff;
  packet[2] = targetPort & 0xff;
  packet[3] = Math.min(payload.byteLength, 60);
  packet.set(payload.slice(0, 60), 4);

  const socket = await Bun.connect({ unix: SOCKET_PATH, socket: {} });
  socket.write(packet);
  socket.end();
}

async function findActiveLog(): Promise<string | null> {
  const logs = await listLogs();
  return logs.length ? join(configState.config.log_directory, logs[0].name) : null;
}

async function pollActiveLog(): Promise<void> {
  try {
    const newest = await findActiveLog();
    if (newest !== activeLogPath) {
      activeLogPath = newest;
      activeLogOffset = newest ? (await stat(newest)).size : 0;
    }
    if (!activeLogPath) return;
    const info = await stat(activeLogPath);
    if (info.size < activeLogOffset) activeLogOffset = 0;
    if (info.size === activeLogOffset) return;

    const file = Bun.file(activeLogPath);
    const chunk = await file.slice(activeLogOffset, info.size).text();
    activeLogOffset = info.size;
    for (const line of chunk.split(/\r?\n/).filter(Boolean)) {
      broadcastLog(line);
    }
    await rotateLogs();
  } catch {
    activeLogPath = null;
    activeLogOffset = 0;
  }
}

function broadcastLog(line: string): void {
  const payload = JSON.stringify({ type: "log", line, at: Date.now() });
  for (const ws of logSockets) ws.send(payload);
}

function broadcastStatus(): void {
  const payload = JSON.stringify({
    type: "status",
    status: latestMainDeviceStatus
  });
  for (const ws of logSockets) ws.send(payload);
}

function broadcastConfig(): void {
  const payload = JSON.stringify({
    type: "config_loaded",
    configState,
    isConfigLoaded: configState.isConfigLoaded
  });
  for (const ws of logSockets) ws.send(payload);
}

async function systemInfo() {
  const [cpuInfo, memInfo, kernel, netDev] = await Promise.all([
    readTextOr("/proc/cpuinfo", ""),
    readTextOr("/proc/meminfo", ""),
    readTextOr("/proc/version", ""),
    readTextOr("/proc/net/dev", "")
  ]);
  const model = cpuInfo.match(/^model name\s*:\s*(.+)$/m)?.[1]
    ?? cpuInfo.match(/^Hardware\s*:\s*(.+)$/m)?.[1]
    ?? "ARM64 Linux controller";
  const memTotalKb = Number(memInfo.match(/^MemTotal:\s+(\d+)/m)?.[1] ?? 0);
  const memAvailableKb = Number(memInfo.match(/^MemAvailable:\s+(\d+)/m)?.[1] ?? 0);
  const networkBytes = netDev.split("\n").slice(2).reduce((total, line) => {
    const parts = line.trim().split(/\s+/);
    if (parts.length < 17) return total;
    return total + Number(parts[1] ?? 0) + Number(parts[9] ?? 0);
  }, 0);
  return {
    model,
    memTotalMb: Math.round(memTotalKb / 1024),
    memAvailableMb: Math.round(memAvailableKb / 1024),
    networkBytes,
    kernel: kernel.trim(),
    preemptRtActive: /PREEMPT_RT|PREEMPT RT/i.test(kernel)
  };
}

async function runSystemRestart(target: string) {
  const serviceMap: Record<string, string[]> = {
    daemon: ["ec_maindevice"],
    dashboard: ["ec_dashboard"],
    all: ["ec_maindevice", "ec_dashboard"]
  };
  const services = serviceMap[target];
  if (!services) return { ok: false, message: "Unknown restart target" };

  for (const service of services) {
    const proc = Bun.spawn(["sudo", "systemctl", "restart", service], {
      stdout: "pipe",
      stderr: "pipe"
    });
    const exitCode = await proc.exited;
    if (exitCode !== 0) {
      const stderr = await new Response(proc.stderr).text();
      return { ok: false, message: stderr || `Restart failed for ${service}` };
    }
  }
  return { ok: true, message: "Restart command accepted" };
}

function zipDateParts(date = new Date()) {
  return {
    time: ((date.getHours() & 0x1f) << 11) | ((date.getMinutes() & 0x3f) << 5) | Math.floor(date.getSeconds() / 2),
    date: (((date.getFullYear() - 1980) & 0x7f) << 9) | (((date.getMonth() + 1) & 0x0f) << 5) | (date.getDate() & 0x1f)
  };
}

const crcTable = new Uint32Array(256).map((_, index) => {
  let value = index;
  for (let bit = 0; bit < 8; bit += 1) {
    value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
  }
  return value >>> 0;
});

function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of data) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function u16(value: number) {
  return new Uint8Array([value & 0xff, (value >>> 8) & 0xff]);
}

function u32(value: number) {
  return new Uint8Array([value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff]);
}

async function makeZip(names: string[]): Promise<Uint8Array> {
  const encoder = new TextEncoder();
  const chunks: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  const stamp = zipDateParts();

  for (const name of names) {
    const path = safeLogPath(name);
    if (!path) continue;
    const data = new Uint8Array(await Bun.file(path).arrayBuffer());
    const fileName = encoder.encode(basename(name));
    const crc = crc32(data);
    const local = concat([
      u32(0x04034b50), u16(20), u16(0), u16(0), u16(stamp.time), u16(stamp.date),
      u32(crc), u32(data.byteLength), u32(data.byteLength), u16(fileName.byteLength), u16(0), fileName
    ]);
    chunks.push(local, data);
    central.push(concat([
      u32(0x02014b50), u16(20), u16(20), u16(0), u16(0), u16(stamp.time), u16(stamp.date),
      u32(crc), u32(data.byteLength), u32(data.byteLength), u16(fileName.byteLength), u16(0), u16(0),
      u16(0), u16(0), u32(0), u32(offset), fileName
    ]));
    offset += local.byteLength + data.byteLength;
  }

  const centralOffset = offset;
  const centralBlock = concat(central);
  const end = concat([
    u32(0x06054b50), u16(0), u16(0), u16(central.length), u16(central.length),
    u32(centralBlock.byteLength), u32(centralOffset), u16(0)
  ]);
  return concat([...chunks, centralBlock, end]);
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((sum, part) => sum + part.byteLength, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

async function serveStatic(pathname: string): Promise<Response> {
  const fileName = pathname === "/" ? "index.html" : pathname.slice(1);
  const resolved = resolve(PUBLIC_DIR, fileName);
  if (!resolved.startsWith(resolve(PUBLIC_DIR))) return text("Forbidden", 403);
  const file = Bun.file(resolved);
  if (!(await file.exists())) return text("Not found", 404);
  return new Response(file, {
    headers: {
      "cache-control": "no-store",
      "content-type": contentType(resolved)
    }
  });
}

function contentType(path: string): string {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
  if (path.endsWith(".css")) return "text/css; charset=utf-8";
  return "application/octet-stream";
}

async function handleApi(req: Request, url: URL): Promise<Response> {
  if (url.pathname === "/api/status") {
    const [adapters, logs, info] = await Promise.all([listAdapters(), listLogs(), systemInfo()]);
    return json({
      dashboardVersion: DASHBOARD_VERSION,
      daemonVersion: DAEMON_VERSION,
      status: latestMainDeviceStatus,
      adapters,
      logCount: logs.length,
      system: info,
      configState,
      isConfigLoaded: configState.isConfigLoaded
    });
  }

  if (url.pathname === "/api/config" && req.method === "GET") {
    return json(configState);
  }

  if (url.pathname === "/api/config" && req.method === "POST") {
    const next = normalizeConfig(await req.json());
    await persistConfig(next, false);
    await rotateLogs();
    return json({ ok: !configState.regeneratedFailsafe, configState });
  }

  if (url.pathname === "/api/config/apply-defaults" && req.method === "POST") {
    await persistConfig(defaultConfig, false);
    await rotateLogs();
    return json({ ok: !configState.regeneratedFailsafe, configState });
  }

  if (url.pathname === "/api/logs" && req.method === "GET") {
    await rotateLogs();
    return json({ logs: await listLogs() });
  }

  if (url.pathname === "/api/logs/delete" && req.method === "POST") {
    const body = await req.json();
    return json({ ok: true, deleted: await deleteLogs(Array.isArray(body.names) ? body.names : []) });
  }

  if (url.pathname === "/api/logs/delete-all" && req.method === "POST") {
    return json({ ok: true, deleted: await deleteAllLogs() });
  }

  if (url.pathname === "/api/logs/download" && req.method === "POST") {
    const body = await req.json();
    const names = Array.isArray(body.names) ? body.names : [];
    const zip = await makeZip(names);
    return new Response(zip, {
      headers: {
        "content-type": "application/zip",
        "content-disposition": `attachment; filename="ec_logs_${Date.now()}.zip"`
      }
    });
  }

  if (url.pathname === "/api/faults/clear" && req.method === "POST") {
    await sendMainDeviceCommand(0x03);
    latestMainDeviceStatus.activeFaults = 0;
    latestMainDeviceStatus.lostFrames = 0;
    return json({ ok: true });
  }

  if (url.pathname === "/api/subdevices/station-id" && req.method === "POST") {
    const body = await req.json();
    const payload = new TextEncoder().encode(String(body.stationId ?? ""));
    await sendMainDeviceCommand(0x02, Number(body.subDeviceIndex ?? 0), 0, payload);
    return json({ ok: true });
  }

  if (url.pathname === "/api/esi/upload" && req.method === "POST") {
    const form = await req.formData();
    const file = form.get("file");
    if (!(file instanceof File)) return json({ ok: false, message: "Missing ESI/XML file" }, { status: 400 });
    await mkdir("/etc/ethercat/esi", { recursive: true });
    const clean = basename(file.name).replace(/[^A-Za-z0-9._-]/g, "_");
    await Bun.write(`/etc/ethercat/esi/${clean}`, file);
    return json({ ok: true, name: clean });
  }

  if (url.pathname === "/api/system/restart" && req.method === "POST") {
    const body = await req.json();
    const result = await runSystemRestart(String(body.target ?? ""));
    return json(result, { status: result.ok ? 200 : 400 });
  }

  return text("Not found", 404);
}

await loadConfig();
await rotateLogs();
void connectStatusSocket();
setInterval(pollActiveLog, 1000);
setInterval(broadcastStatus, 1000);

Bun.serve({
  port: PORT,
  async fetch(req, server) {
    const url = new URL(req.url);
    if (url.pathname === "/ws/logs") {
      if (server.upgrade(req)) return undefined;
      return text("WebSocket upgrade failed", 400);
    }
    if (url.pathname.startsWith("/api/")) {
      try {
        return await handleApi(req, url);
      } catch (error) {
        return json({
          ok: false,
          message: error instanceof Error ? error.message : "Request failed"
        }, { status: 500 });
      }
    }
    return serveStatic(url.pathname);
  },
  websocket: {
    open(ws) {
      logSockets.add(ws);
      ws.send(JSON.stringify({
        type: "status",
        status: latestMainDeviceStatus
      }));
      ws.send(JSON.stringify({
        type: "config_loaded",
        configState,
        isConfigLoaded: configState.isConfigLoaded
      }));
    },
    close(ws) {
      logSockets.delete(ws);
    },
    message() {
      // The dashboard uses WebSocket receive-only log streaming.
    }
  }
});

console.log(`EC Dashboard ${DASHBOARD_VERSION} listening on port ${PORT}`);
