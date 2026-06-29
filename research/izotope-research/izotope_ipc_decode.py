#!/usr/bin/env python3
"""
Read-only iZotope IPC discovery and payload capture.

This attaches Frida to the running DAW process, hooks common Windows IPC and
socket APIs, and writes observed metadata/payloads to JSONL for offline decoding.
It does not write into the target process and does not send network data.

Typical use:
  python tools/izotope_ipc_decode.py --pid 24352 --duration 45

Start the capture, then trigger the Ozone/Neutron/Relay action in the UI while
the timer is running.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any


JS_TEMPLATE = r"""
'use strict';

const MAX_BYTES = __MAX_BYTES__;
const INCLUDE_MOREPHI = __INCLUDE_MOREPHI__;
const INCLUDE_BACKTRACE = __INCLUDE_BACKTRACE__;

const socketInfo = {};
const handleInfo = {};
const internetInfo = {};

function nowMs() {
  return Date.now();
}

function safeUtf16(ptr) {
  try {
    if (ptr.isNull()) return '';
    return ptr.readUtf16String() || '';
  } catch (_) {
    return '<unreadable>';
  }
}

function safeUtf8(ptr) {
  try {
    if (ptr.isNull()) return '';
    return ptr.readUtf8String() || '';
  } catch (_) {
    return '<unreadable>';
  }
}

function isInterestingName(s) {
  return /izotope|ozone|neutron|relay|assistant|visual|tonal|ipc|pipe|zmq|nano|nng|morephi/i.test(s || '');
}

function looksLikeMorePhiEndpoint(s) {
  return /127\.0\.0\.1:(3000[0-9]|3001[0-9])/.test(s || '');
}

function shouldBacktrace(kind) {
  return INCLUDE_BACKTRACE && /CreateFile|ReadFile|WriteFile|FileMapping|MapView|NamedPipe|socket\.|wininet\./.test(kind || '');
}

function captureBacktrace(context) {
  if (!context) return null;
  try {
    return Thread.backtrace(context, Backtracer.ACCURATE)
      .slice(0, 24)
      .map(function (addr) {
        const sym = DebugSymbol.fromAddress(addr);
        return {
          address: String(addr),
          module: sym.moduleName || '',
          name: sym.name || '',
          symbol: String(sym)
        };
      });
  } catch (e) {
    return [{ error: String(e) }];
  }
}

function emit(kind, data, ptr, len, context) {
  data = data || {};
  data.kind = kind;
  data.ts_ms = nowMs();
  if (shouldBacktrace(kind) && !data.backtrace) {
    const bt = captureBacktrace(context);
    if (bt) data.backtrace = bt;
  }
  if (ptr && len && len > 0) {
    const n = Math.min(Number(len), MAX_BYTES);
    try {
      send(data, ptr.readByteArray(n));
      return;
    } catch (e) {
      data.read_error = String(e);
    }
  }
  send(data);
}

function attachExport(moduleName, exportName, callbacks) {
  let addr = null;
  try {
    if (moduleName) {
      const mod = Process.getModuleByName(moduleName);
      if (mod && mod.findExportByName) {
        addr = mod.findExportByName(exportName);
      }
    } else if (Module.findGlobalExportByName) {
      addr = Module.findGlobalExportByName(exportName);
    }
  } catch (_) {
    addr = null;
  }
  if (addr === null) {
    try {
      if (Module.findExportByName) {
        addr = Module.findExportByName(moduleName, exportName);
      }
    } catch (_) {
      addr = null;
    }
  }
  if (addr === null) return false;
  try {
    Interceptor.attach(addr, callbacks);
    emit('hooked', { export: (moduleName || '*') + '!' + exportName, address: String(addr) });
    return true;
  } catch (e) {
    emit('hook_failed', { export: (moduleName || '*') + '!' + exportName, error: String(e) });
    return false;
  }
}

function parseSockaddr(sockaddr) {
  try {
    if (sockaddr.isNull()) return '<null>';
    const family = sockaddr.readU16();
    if (family === 2) {
      const netPort = sockaddr.add(2).readU16();
      const port = ((netPort & 0xff) << 8) | ((netPort >> 8) & 0xff);
      const b0 = sockaddr.add(4).readU8();
      const b1 = sockaddr.add(5).readU8();
      const b2 = sockaddr.add(6).readU8();
      const b3 = sockaddr.add(7).readU8();
      return b0 + '.' + b1 + '.' + b2 + '.' + b3 + ':' + port;
    }
    if (family === 23) {
      const netPort = sockaddr.add(2).readU16();
      const port = ((netPort & 0xff) << 8) | ((netPort >> 8) & 0xff);
      return 'ipv6:' + port;
    }
    return 'family=' + family;
  } catch (e) {
    return '<sockaddr unreadable: ' + e + '>';
  }
}

function shouldLogSocket(sock) {
  if (INCLUDE_MOREPHI) return true;
  const info = socketInfo[String(sock)];
  if (!info) return true;
  return !looksLikeMorePhiEndpoint(info.endpoint || '');
}

function readUlongPtr(ptr) {
  try {
    if (ptr.isNull()) return 0;
    return ptr.readU32();
  } catch (_) {
    return 0;
  }
}

function pointerString(p) {
  try {
    if (p.isNull()) return '0x0';
  } catch (_) {}
  return String(p);
}

function hookKernelObjects() {
  attachExport('kernel32.dll', 'CreateFileMappingW', {
    onEnter(args) { this.name = safeUtf16(args[5]); },
    onLeave(retval) {
      if (this.name) emit('CreateFileMappingW', { name: this.name, retval: String(retval) });
    }
  });
  attachExport('kernel32.dll', 'OpenFileMappingW', {
    onEnter(args) { this.name = safeUtf16(args[2]); },
    onLeave(retval) {
      if (this.name) emit('OpenFileMappingW', { name: this.name, retval: String(retval) });
    }
  });
  attachExport('kernel32.dll', 'MapViewOfFile', {
    onEnter(args) { this.handle = String(args[0]); this.size = args[4].toUInt32 ? args[4].toUInt32() : 0; },
    onLeave(retval) {
      const requested = this.size;
      const probeLen = requested > 0 ? Math.min(requested, MAX_BYTES) : Math.min(4096, MAX_BYTES);
      if (!retval.isNull() && probeLen > 0) {
        emit('MapViewOfFile', { handle: this.handle, size: requested, retval: String(retval), probe_len: probeLen }, retval, probeLen, this.context);
      } else {
        emit('MapViewOfFile', { handle: this.handle, size: requested, retval: String(retval) }, null, 0, this.context);
      }
    }
  });
  attachExport('kernel32.dll', 'CreateMutexW', {
    onEnter(args) { this.name = safeUtf16(args[2]); },
    onLeave(retval) {
      if (this.name) emit('CreateMutexW', { name: this.name, retval: String(retval) });
    }
  });
  attachExport('kernel32.dll', 'OpenMutexW', {
    onEnter(args) { this.name = safeUtf16(args[2]); },
    onLeave(retval) {
      if (this.name) emit('OpenMutexW', { name: this.name, retval: String(retval) });
    }
  });
  attachExport('kernel32.dll', 'CreateEventW', {
    onEnter(args) { this.name = safeUtf16(args[3]); },
    onLeave(retval) {
      if (this.name) emit('CreateEventW', { name: this.name, retval: String(retval) });
    }
  });
  attachExport('kernel32.dll', 'CreateNamedPipeW', {
    onEnter(args) { this.name = safeUtf16(args[0]); },
    onLeave(retval) {
      if (this.name) {
        handleInfo[String(retval)] = this.name;
        emit('CreateNamedPipeW', { name: this.name, handle: String(retval) });
      }
    }
  });
  attachExport('kernel32.dll', 'CreateFileW', {
    onEnter(args) { this.name = safeUtf16(args[0]); },
    onLeave(retval) {
      if (this.name && (isInterestingName(this.name) || this.name.indexOf('\\\\.\\pipe\\') === 0)) {
        handleInfo[String(retval)] = this.name;
        emit('CreateFileW', { name: this.name, handle: String(retval) });
      }
    }
  });
  attachExport('kernel32.dll', 'WriteFile', {
    onEnter(args) {
      this.handle = String(args[0]);
      this.buf = args[1];
      this.len = args[2].toUInt32();
      this.name = handleInfo[this.handle] || '';
    },
    onLeave(retval) {
      if (this.name && isInterestingName(this.name)) {
        emit('WriteFile', { handle: this.handle, name: this.name, retval: String(retval), length: this.len }, this.buf, this.len, this.context);
      }
    }
  });
  attachExport('kernel32.dll', 'ReadFile', {
    onEnter(args) {
      this.handle = String(args[0]);
      this.buf = args[1];
      this.requested = args[2].toUInt32();
      this.bytesReadPtr = args[3];
      this.name = handleInfo[this.handle] || '';
    },
    onLeave(retval) {
      if (this.name && isInterestingName(this.name)) {
        const got = readUlongPtr(this.bytesReadPtr);
        if (got > 0) emit('ReadFile', { handle: this.handle, name: this.name, retval: String(retval), length: got, requested: this.requested }, this.buf, got, this.context);
      }
    }
  });
}

function hookSockets() {
  attachExport('ws2_32.dll', 'bind', {
    onEnter(args) {
      const endpoint = parseSockaddr(args[1]);
      socketInfo[String(args[0])] = { direction: 'bind', endpoint: endpoint };
      emit('socket.bind', { socket: String(args[0]), endpoint: endpoint }, null, 0, this.context);
    }
  });
  attachExport('ws2_32.dll', 'connect', {
    onEnter(args) {
      const endpoint = parseSockaddr(args[1]);
      socketInfo[String(args[0])] = { direction: 'connect', endpoint: endpoint };
      emit('socket.connect', { socket: String(args[0]), endpoint: endpoint }, null, 0, this.context);
    }
  });
  attachExport('ws2_32.dll', 'send', {
    onEnter(args) {
      const sock = String(args[0]);
      const len = args[2].toInt32();
      if (len > 0 && shouldLogSocket(sock)) {
        emit('socket.send', { socket: sock, endpoint: socketInfo[sock] || null, length: len }, args[1], len, this.context);
      }
    }
  });
  attachExport('ws2_32.dll', 'recv', {
    onEnter(args) { this.sock = String(args[0]); this.buf = args[1]; },
    onLeave(retval) {
      const len = retval.toInt32();
      if (len > 0 && shouldLogSocket(this.sock)) {
        emit('socket.recv', { socket: this.sock, endpoint: socketInfo[this.sock] || null, length: len }, this.buf, len, this.context);
      }
    }
  });
  attachExport('ws2_32.dll', 'sendto', {
    onEnter(args) {
      const sock = String(args[0]);
      const len = args[2].toInt32();
      const endpoint = parseSockaddr(args[4]);
      if (len > 0 && shouldLogSocket(sock)) {
        emit('socket.sendto', { socket: sock, endpoint: endpoint, length: len }, args[1], len, this.context);
      }
    }
  });
  attachExport('ws2_32.dll', 'recvfrom', {
    onEnter(args) { this.sock = String(args[0]); this.buf = args[1]; this.from = args[4]; },
    onLeave(retval) {
      const len = retval.toInt32();
      if (len > 0 && shouldLogSocket(this.sock)) {
        emit('socket.recvfrom', { socket: this.sock, endpoint: parseSockaddr(this.from), length: len }, this.buf, len, this.context);
      }
    }
  });
  attachExport('ws2_32.dll', 'WSASend', {
    onEnter(args) {
      const sock = String(args[0]);
      if (!shouldLogSocket(sock)) return;
      const buffers = args[1];
      const count = args[2].toUInt32();
      for (let i = 0; i < Math.min(count, 4); ++i) {
        const item = buffers.add(i * Process.pointerSize * 2);
        const len = item.readU32();
        const ptr = item.add(Process.pointerSize).readPointer();
        if (len > 0) emit('socket.WSASend', { socket: sock, endpoint: socketInfo[sock] || null, index: i, length: len }, ptr, len, this.context);
      }
    }
  });
  attachExport('ws2_32.dll', 'WSARecv', {
    onEnter(args) {
      this.sock = String(args[0]);
      this.buffers = args[1];
      this.count = args[2].toUInt32();
      this.bytesPtr = args[3];
    },
    onLeave(retval) {
      if (!shouldLogSocket(this.sock)) return;
      const got = readUlongPtr(this.bytesPtr);
      if (got <= 0) return;
      const item = this.buffers;
      const len = Math.min(item.readU32(), got);
      const ptr = item.add(Process.pointerSize).readPointer();
      if (len > 0) emit('socket.WSARecv', { socket: this.sock, endpoint: socketInfo[this.sock] || null, length: len, total: got }, ptr, len, this.context);
    }
  });
}

function hookWinInet() {
  function saveInternetHandle(retval, info) {
    const key = String(retval);
    if (key !== '0x0' && key !== '-1') internetInfo[key] = info;
  }

  attachExport('wininet.dll', 'InternetConnectA', {
    onEnter(args) {
      this.server = safeUtf8(args[1]);
      this.port = args[2].toUInt32();
    },
    onLeave(retval) {
      saveInternetHandle(retval, { server: this.server, port: this.port });
      emit('wininet.InternetConnectA', { server: this.server, port: this.port, handle: String(retval) }, null, 0, this.context);
    }
  });
  attachExport('wininet.dll', 'InternetConnectW', {
    onEnter(args) {
      this.server = safeUtf16(args[1]);
      this.port = args[2].toUInt32();
    },
    onLeave(retval) {
      saveInternetHandle(retval, { server: this.server, port: this.port });
      emit('wininet.InternetConnectW', { server: this.server, port: this.port, handle: String(retval) }, null, 0, this.context);
    }
  });
  attachExport('wininet.dll', 'InternetOpenUrlA', {
    onEnter(args) {
      this.url = safeUtf8(args[1]);
      this.headers = safeUtf8(args[2]);
    },
    onLeave(retval) {
      saveInternetHandle(retval, { url: this.url });
      emit('wininet.InternetOpenUrlA', { url: this.url, headers: this.headers, handle: String(retval) }, null, 0, this.context);
    }
  });
  attachExport('wininet.dll', 'InternetOpenUrlW', {
    onEnter(args) {
      this.url = safeUtf16(args[1]);
      this.headers = safeUtf16(args[2]);
    },
    onLeave(retval) {
      saveInternetHandle(retval, { url: this.url });
      emit('wininet.InternetOpenUrlW', { url: this.url, headers: this.headers, handle: String(retval) }, null, 0, this.context);
    }
  });
  attachExport('wininet.dll', 'HttpOpenRequestA', {
    onEnter(args) {
      this.parent = String(args[0]);
      this.verb = safeUtf8(args[1]);
      this.object = safeUtf8(args[2]);
    },
    onLeave(retval) {
      const base = internetInfo[this.parent] || {};
      saveInternetHandle(retval, Object.assign({}, base, { verb: this.verb, object: this.object }));
      emit('wininet.HttpOpenRequestA', { parent: this.parent, base: base, verb: this.verb, object: this.object, handle: String(retval) }, null, 0, this.context);
    }
  });
  attachExport('wininet.dll', 'HttpOpenRequestW', {
    onEnter(args) {
      this.parent = String(args[0]);
      this.verb = safeUtf16(args[1]);
      this.object = safeUtf16(args[2]);
    },
    onLeave(retval) {
      const base = internetInfo[this.parent] || {};
      saveInternetHandle(retval, Object.assign({}, base, { verb: this.verb, object: this.object }));
      emit('wininet.HttpOpenRequestW', { parent: this.parent, base: base, verb: this.verb, object: this.object, handle: String(retval) }, null, 0, this.context);
    }
  });
  attachExport('wininet.dll', 'HttpSendRequestA', {
    onEnter(args) {
      this.handle = String(args[0]);
      this.headers = safeUtf8(args[1]);
      this.optional = args[3];
      this.optionalLen = args[4].toUInt32();
    },
    onLeave(retval) {
      emit('wininet.HttpSendRequestA', { handle: this.handle, info: internetInfo[this.handle] || null, headers: this.headers, retval: String(retval), length: this.optionalLen }, this.optional, this.optionalLen, this.context);
    }
  });
  attachExport('wininet.dll', 'HttpSendRequestW', {
    onEnter(args) {
      this.handle = String(args[0]);
      this.headers = safeUtf16(args[1]);
      this.optional = args[3];
      this.optionalLen = args[4].toUInt32();
    },
    onLeave(retval) {
      emit('wininet.HttpSendRequestW', { handle: this.handle, info: internetInfo[this.handle] || null, headers: this.headers, retval: String(retval), length: this.optionalLen }, this.optional, this.optionalLen, this.context);
    }
  });
  attachExport('wininet.dll', 'InternetReadFile', {
    onEnter(args) {
      this.handle = String(args[0]);
      this.buf = args[1];
      this.requested = args[2].toUInt32();
      this.bytesReadPtr = args[3];
    },
    onLeave(retval) {
      const got = readUlongPtr(this.bytesReadPtr);
      if (got > 0) {
        emit('wininet.InternetReadFile', { handle: this.handle, info: internetInfo[this.handle] || null, retval: String(retval), length: got, requested: this.requested }, this.buf, got, this.context);
      }
    }
  });
}

function hookMessagingLibraries() {
  [
    ['nanomsg.dll', 'nn_bind', 1],
    ['nanomsg.dll', 'nn_connect', 1],
    [null, 'nn_bind', 1],
    [null, 'nn_connect', 1],
    [null, 'nn_send', 1],
    [null, 'nn_recv', 1],
    ['libzmq.dll', 'zmq_bind', 1],
    ['libzmq.dll', 'zmq_connect', 1],
    ['libzmq.dll', 'zmq_send', 1],
    ['libzmq.dll', 'zmq_recv', 1],
    ['zmq.dll', 'zmq_bind', 1],
    ['zmq.dll', 'zmq_connect', 1],
    ['zmq.dll', 'zmq_send', 1],
    ['zmq.dll', 'zmq_recv', 1],
    [null, 'zmq_bind', 1],
    [null, 'zmq_connect', 1],
    [null, 'zmq_send', 1],
    [null, 'zmq_recv', 1],
  ].forEach(function (spec) {
    const name = spec[1];
    if (/_bind$|_connect$/.test(name)) {
      attachExport(spec[0], name, {
        onEnter(args) { emit(name, { endpoint: safeUtf8(args[spec[2]]) }); }
      });
    } else if (/_send$/.test(name)) {
      attachExport(spec[0], name, {
        onEnter(args) {
          const len = args[2].toInt32();
          if (len > 0) emit(name, { socket: String(args[0]), length: len }, args[1], len);
        }
      });
    } else if (/_recv$/.test(name)) {
      attachExport(spec[0], name, {
        onEnter(args) { this.socket = String(args[0]); this.buf = args[1]; },
        onLeave(retval) {
          const len = retval.toInt32();
          if (len > 0) emit(name, { socket: this.socket, length: len }, this.buf, len);
        }
      });
    }
  });
}

function hookLoadLibrary() {
  attachExport('kernel32.dll', 'LoadLibraryW', {
    onEnter(args) { this.name = safeUtf16(args[0]); },
    onLeave(retval) { if (this.name && isInterestingName(this.name)) emit('LoadLibraryW', { name: this.name, retval: String(retval) }); }
  });
  attachExport('kernel32.dll', 'LoadLibraryA', {
    onEnter(args) { this.name = safeUtf8(args[0]); },
    onLeave(retval) { if (this.name && isInterestingName(this.name)) emit('LoadLibraryA', { name: this.name, retval: String(retval) }); }
  });
}

function printModules() {
  Process.enumerateModules().forEach(function (m) {
    const nonSystem = m.path && !/\\Windows\\|\\Microsoft\\/i.test(m.path);
    if (isInterestingName(m.name) || isInterestingName(m.path) || nonSystem) {
      emit('module', { name: m.name, path: m.path, base: String(m.base), size: m.size });
    }
  });
}

emit('trace.start', { pid: Process.id, max_bytes: MAX_BYTES, include_morephi: INCLUDE_MOREPHI });
printModules();
hookLoadLibrary();
hookKernelObjects();
hookSockets();
hookWinInet();
hookMessagingLibraries();
emit('trace.ready', { message: 'Trigger Ozone/Neutron/Relay IPC actions now.' });
"""


def ascii_preview(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data[:256])


def hex_preview(data: bytes) -> str:
    return data[:96].hex(" ")


def classify_payload(data: bytes) -> list[str]:
    tags: list[str] = []
    if not data:
        return tags
    if data.startswith(b"\xff"):
        tags.append("zmq_zmtp_like")
    if data.startswith(b"\x00SP\x00") or b"\x00SP\x00" in data[:32]:
        tags.append("nanomsg_sp_like")
    stripped = data.lstrip()
    if stripped.startswith((b"{", b"[")):
        tags.append("json_like")
    if b"<?xml" in data[:128] or b"<" in data[:16] and b">" in data[:128]:
        tags.append("xml_like")
    for word in (b"iZotope", b"Ozone", b"Neutron", b"Relay", b"Assistant", b"IPC", b"IZOT"):
        if word.lower() in data.lower():
            tags.append(word.decode("ascii", "ignore").lower())
    zero_ratio = data.count(0) / max(1, len(data))
    if zero_ratio > 0.3:
        tags.append("binary")
    if len(set(data[: min(len(data), 64)])) < 8:
        tags.append("low_entropy")
    return tags


def make_js(max_bytes: int, include_morephi: bool, include_backtrace: bool) -> str:
    return (
        JS_TEMPLATE.replace("__MAX_BYTES__", str(max_bytes))
        .replace("__INCLUDE_MOREPHI__", "true" if include_morephi else "false")
        .replace("__INCLUDE_BACKTRACE__", "true" if include_backtrace else "false")
    )


def write_event(fp, message: dict[str, Any], data: bytes | None) -> dict[str, Any]:
    payload = message.get("payload", {}) if message.get("type") == "send" else {}
    if not isinstance(payload, dict):
        payload = {"payload": payload}
    if data:
        payload["payload_len"] = len(data)
        payload["payload_b64"] = base64.b64encode(data).decode("ascii")
        payload["payload_hex_preview"] = hex_preview(data)
        payload["payload_ascii_preview"] = ascii_preview(data)
        payload["payload_tags"] = classify_payload(data)
    fp.write(json.dumps(payload, separators=(",", ":")) + "\n")
    fp.flush()
    return payload


def summarize(path: Path) -> dict[str, Any]:
    counts = Counter()
    endpoints = defaultdict(Counter)
    tags = Counter()
    names = Counter()
    payload_events = 0

    with path.open("r", encoding="utf-8") as fp:
        for line in fp:
            if not line.strip():
                continue
            ev = json.loads(line)
            kind = ev.get("kind", "")
            counts[kind] += 1
            if "name" in ev:
                names[str(ev["name"])] += 1
            endpoint = ev.get("endpoint")
            if isinstance(endpoint, dict):
                endpoint = endpoint.get("endpoint")
            if endpoint:
                endpoints[str(endpoint)][kind] += 1
            for tag in ev.get("payload_tags", []) or []:
                tags[tag] += 1
            if ev.get("payload_len"):
                payload_events += 1

    return {
        "events": dict(counts.most_common()),
        "payload_events": payload_events,
        "payload_tags": dict(tags.most_common()),
        "endpoints": {k: dict(v.most_common()) for k, v in endpoints.items()},
        "names": dict(names.most_common(40)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="Target process PID. Defaults to first FL64.exe.")
    parser.add_argument("--duration", type=float, default=45.0, help="Capture duration in seconds.")
    parser.add_argument("--out-dir", default="tools/live_captures/ipc_decode", help="Output directory.")
    parser.add_argument("--max-bytes", type=int, default=768, help="Max payload bytes per event.")
    parser.add_argument("--include-morephi", action="store_true", help="Do not suppress More-Phi MCP socket payloads.")
    parser.add_argument("--backtrace", action="store_true", help="Include native backtraces for file/map/socket events.")
    args = parser.parse_args()

    try:
        import frida  # type: ignore
    except Exception as exc:
        print(f"frida Python package is not available: {exc}", file=sys.stderr)
        print("Install with: python -m pip install --user frida-tools", file=sys.stderr)
        return 2

    if args.pid is None:
        import subprocess

        ps = subprocess.check_output(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "(Get-Process FL64 -ErrorAction Stop | Select-Object -First 1).Id",
            ],
            text=True,
        ).strip()
        args.pid = int(ps)

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    jsonl_path = out_dir / f"izotope_ipc_decode_{stamp}.jsonl"
    summary_path = out_dir / f"izotope_ipc_decode_{stamp}_summary.json"

    print(f"Attaching to PID {args.pid}")
    print(f"Writing events to {jsonl_path}")
    print("Trigger the iZotope action now while capture is running.")

    device = frida.get_local_device()
    session = device.attach(args.pid)
    script = session.create_script(make_js(args.max_bytes, args.include_morephi, args.backtrace))

    stopped = False

    def stop_now(*_: Any) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_now)

    with jsonl_path.open("w", encoding="utf-8") as fp:
        def on_message(message: dict[str, Any], data: bytes | None) -> None:
            payload = write_event(fp, message, data)
            kind = payload.get("kind")
            if kind in {"trace.ready", "CreateFileMappingW", "OpenFileMappingW", "CreateNamedPipeW", "CreateFileW", "socket.bind", "socket.connect", "zmq_bind", "zmq_connect", "nn_bind", "nn_connect"}:
                print(json.dumps({k: payload.get(k) for k in ("kind", "name", "endpoint", "export", "message", "retval") if k in payload}, separators=(",", ":")))

        script.on("message", on_message)
        script.load()

        end = time.time() + max(0.1, args.duration)
        while not stopped and time.time() < end:
            time.sleep(0.2)

    try:
        script.unload()
    finally:
        session.detach()

    summary = summarize(jsonl_path)
    summary["pid"] = args.pid
    summary["duration_seconds"] = args.duration
    summary["jsonl_path"] = str(jsonl_path)
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print(f"Summary written to {summary_path}")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
