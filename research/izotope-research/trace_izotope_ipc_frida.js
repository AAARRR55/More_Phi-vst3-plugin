/*
 * Frida trace for iZotope IPC transport discovery.
 *
 * Usage:
 *   frida -p <FL64_PID> -l tools/trace_izotope_ipc_frida.js
 *
 * This script is observational only. It hooks common transport and kernel-object
 * APIs and prints names/endpoints seen during Ozone/Neutron/Relay IPC startup.
 */

'use strict';

function safeReadUtf16(ptr) {
  try {
    if (ptr.isNull()) return '';
    return ptr.readUtf16String();
  } catch (_) {
    return '<unreadable>';
  }
}

function safeReadUtf8(ptr) {
  try {
    if (ptr.isNull()) return '';
    return ptr.readUtf8String();
  } catch (_) {
    return '<unreadable>';
  }
}

function interesting(s) {
  return /izotope|ozone|neutron|relay|assistant|ipc|pipe|zmq|nano|visual/i.test(s || '');
}

function logIfInteresting(tag, value) {
  if (interesting(value)) {
    console.log(tag + ' ' + value);
  }
}

function attachExport(moduleName, exportName, callbacks) {
  let addr = null;
  try {
    addr = Module.findExportByName(moduleName, exportName);
  } catch (_) {
    addr = null;
  }
  if (addr === null) return false;
  Interceptor.attach(addr, callbacks);
  console.log('[hooked] ' + (moduleName || '*') + '!' + exportName + ' @ ' + addr);
  return true;
}

function parseSockaddr(sockaddr) {
  try {
    if (sockaddr.isNull()) return '<null>';
    const family = sockaddr.readU16();
    if (family === 2) { // AF_INET
      const netPort = sockaddr.add(2).readU16();
      const port = ((netPort & 0xff) << 8) | ((netPort >> 8) & 0xff);
      const b0 = sockaddr.add(4).readU8();
      const b1 = sockaddr.add(5).readU8();
      const b2 = sockaddr.add(6).readU8();
      const b3 = sockaddr.add(7).readU8();
      return 'AF_INET ' + b0 + '.' + b1 + '.' + b2 + '.' + b3 + ':' + port;
    }
    if (family === 23) { // AF_INET6
      const netPort = sockaddr.add(2).readU16();
      const port = ((netPort & 0xff) << 8) | ((netPort >> 8) & 0xff);
      return 'AF_INET6 port=' + port;
    }
    return 'family=' + family;
  } catch (e) {
    return '<sockaddr unreadable: ' + e + '>';
  }
}

function hookKernelObjects() {
  attachExport('kernel32.dll', 'CreateFileMappingW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[5]);
    },
    onLeave(retval) {
      if (this.name) logIfInteresting('[CreateFileMappingW]', this.name + ' => ' + retval);
    }
  });

  attachExport('kernel32.dll', 'OpenFileMappingW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[2]);
    },
    onLeave(retval) {
      if (this.name) logIfInteresting('[OpenFileMappingW]', this.name + ' => ' + retval);
    }
  });

  attachExport('kernel32.dll', 'CreateMutexW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[2]);
    },
    onLeave(retval) {
      if (this.name) logIfInteresting('[CreateMutexW]', this.name + ' => ' + retval);
    }
  });

  attachExport('kernel32.dll', 'OpenMutexW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[2]);
    },
    onLeave(retval) {
      if (this.name) logIfInteresting('[OpenMutexW]', this.name + ' => ' + retval);
    }
  });

  attachExport('kernel32.dll', 'CreateEventW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[3]);
    },
    onLeave(retval) {
      if (this.name) logIfInteresting('[CreateEventW]', this.name + ' => ' + retval);
    }
  });

  attachExport('kernel32.dll', 'CreateNamedPipeW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[0]);
    },
    onLeave(retval) {
      if (this.name) logIfInteresting('[CreateNamedPipeW]', this.name + ' => ' + retval);
    }
  });

  attachExport('kernel32.dll', 'CreateFileW', {
    onEnter(args) {
      this.name = safeReadUtf16(args[0]);
    },
    onLeave(retval) {
      if (interesting(this.name)) {
        console.log('[CreateFileW] ' + this.name + ' => ' + retval);
      }
    }
  });
}

function hookSockets() {
  attachExport('ws2_32.dll', 'bind', {
    onEnter(args) {
      console.log('[ws2_32!bind] socket=' + args[0] + ' addr=' + parseSockaddr(args[1]));
    }
  });

  attachExport('ws2_32.dll', 'connect', {
    onEnter(args) {
      console.log('[ws2_32!connect] socket=' + args[0] + ' addr=' + parseSockaddr(args[1]));
    }
  });
}

function hookMessagingLibraries() {
  [
    ['nanomsg.dll', 'nn_bind', 1],
    ['nanomsg.dll', 'nn_connect', 1],
    [null, 'nn_bind', 1],
    [null, 'nn_connect', 1],
    ['libzmq.dll', 'zmq_bind', 1],
    ['libzmq.dll', 'zmq_connect', 1],
    ['zmq.dll', 'zmq_bind', 1],
    ['zmq.dll', 'zmq_connect', 1],
    [null, 'zmq_bind', 1],
    [null, 'zmq_connect', 1],
  ].forEach(function (spec) {
    attachExport(spec[0], spec[1], {
      onEnter(args) {
        console.log('[' + spec[1] + '] endpoint=' + safeReadUtf8(args[spec[2]]));
      }
    });
  });
}

function printInterestingModules() {
  Process.enumerateModules()
    .filter(function (m) { return interesting(m.name) || interesting(m.path); })
    .forEach(function (m) {
      console.log('[module] ' + m.name + ' base=' + m.base + ' path=' + m.path);
    });
}

console.log('[trace] iZotope IPC transport trace loaded in pid=' + Process.id);
printInterestingModules();
hookKernelObjects();
hookSockets();
hookMessagingLibraries();
console.log('[trace] Open Ozone/Neutron/Relay UI and trigger Assistant/IPC actions now.');
