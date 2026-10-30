// Utilidades compartidas por las pruebas: arrancar un programa y esperar su «LISTO <puerto>», y hablar HTTP/WebSocket a mano.
import { spawn } from 'node:child_process';
import net from 'node:net';
import crypto from 'node:crypto';

/** Fuera de Linux no hay `xvfb-run` ni hace falta (hay pantalla): se quita y se lanza el programa directamente. */
export function sinXvfb(exe, args) {
  if (exe !== 'xvfb-run' || process.platform === 'linux') return [exe, args];
  let i = 0;
  while (i < args.length && args[i].startsWith('-')) i += args[i] === '-s' ? 2 : 1;
  return [args[i], args.slice(i + 1)];
}

export function arrancar(exe0, args0, { env = {}, esperar = /^LISTO (\d+)/m, plazoMs = 30000, grupo = false } = {}) {
  const [exe, args] = sinXvfb(exe0, args0);
  if (process.platform === 'win32') grupo = false;
  return new Promise((resolve, reject) => {
    const hijo = spawn(exe, args, { env: { ...process.env, ...env }, stdio: ['pipe', 'pipe', 'pipe'], detached: grupo });
    hijo.grupo = grupo;
    let salida = '';
    let errores = '';
    const temporizador = setTimeout(() => {
      matar(hijo);
      reject(new Error(`no arrancó a tiempo.\nsalida: ${salida}\nerrores: ${errores}`));
    }, plazoMs);
    hijo.stdout.on('data', (d) => {
      salida += d.toString();
      const m = salida.match(esperar);
      if (m) {
        clearTimeout(temporizador);
        resolve({ hijo, puerto: Number(m[1]), salida: () => salida, errores: () => errores });
      }
    });
    hijo.stderr.on('data', (d) => { errores += d.toString(); });
    hijo.on('exit', (codigo, senal) => {
      clearTimeout(temporizador);
      reject(new Error(`terminó antes de tiempo (codigo=${codigo}, senal=${senal}).\nsalida: ${salida}\nerrores: ${errores}`));
    });
  });
}

function matar(hijo) {
  try { if (hijo.grupo) process.kill(-hijo.pid, 'SIGKILL'); else hijo.kill('SIGKILL'); } catch { /* ya terminó */ }
}

export function parar(proceso) {
  return new Promise((resolve) => {
    if (!proceso || proceso.hijo.exitCode !== null) return resolve();
    proceso.hijo.removeAllListeners('exit');
    proceso.hijo.once('exit', () => resolve());
    proceso.hijo.stdin.end();
    setTimeout(() => matar(proceso.hijo), 6000).unref();
  });
}

/** Abre un socket TCP crudo, manda `texto` o `bytes` y devuelve lo que llegue hasta que se cierre o pase `ms`. */
export function crudo(puerto, enviar, { ms = 1500 } = {}) {
  return new Promise((resolve) => {
    const s = net.connect(puerto, '127.0.0.1');
    const trozos = [];
    let cerrado = false;
    const fin = () => { if (cerrado) return; cerrado = true; s.destroy(); resolve(Buffer.concat(trozos)); };
    s.on('connect', () => { if (enviar) s.write(enviar); });
    s.on('data', (d) => trozos.push(d));
    s.on('close', fin);
    s.on('error', fin);
    setTimeout(fin, ms).unref();
  });
}

export function peticion({ ruta = '/eco', host, origen, clave, version = '13', extra = '' } = {}) {
  const k = clave ?? crypto.randomBytes(16).toString('base64');
  let t = `GET ${ruta} HTTP/1.1\r\n`;
  if (host !== null) t += `Host: ${host}\r\n`;
  t += 'Upgrade: websocket\r\nConnection: Upgrade\r\n';
  if (origen !== undefined && origen !== null) t += `Origin: ${origen}\r\n`;
  t += `Sec-WebSocket-Key: ${k}\r\nSec-WebSocket-Version: ${version}\r\n${extra}\r\n`;
  return t;
}

/** Trama de cliente (enmascarada salvo que se pida lo contrario). */
export function trama({ opcode = 1, fin = true, carga = Buffer.alloc(0), enmascarar = true, rsv = 0, largoDeclarado } = {}) {
  const n = largoDeclarado ?? carga.length;
  const cab = [];
  cab.push((fin ? 0x80 : 0) | (rsv << 4) | opcode);
  const m = enmascarar ? 0x80 : 0;
  if (n < 126) cab.push(m | n);
  else if (n <= 0xffff) cab.push(m | 126, n >> 8, n & 255);
  else {
    const b = Buffer.alloc(8);
    b.writeBigUInt64BE(BigInt(n));
    cab.push(m | 127, ...b);
  }
  let clave = Buffer.alloc(0);
  let cuerpo = carga;
  if (enmascarar) {
    clave = crypto.randomBytes(4);
    cuerpo = Buffer.from(carga.map((x, i) => x ^ clave[i % 4]));
  }
  return Buffer.concat([Buffer.from(cab), clave, cuerpo]);
}

/** Separa lo que devuelve el servidor tras un handshake: cabeceras HTTP y tramas (sin máscara) que siguen. */
export function leerRespuesta(buf) {
  const i = buf.indexOf('\r\n\r\n');
  if (i < 0) return { cabeceras: buf.toString(), tramas: [] };
  const cabeceras = buf.subarray(0, i).toString();
  let resto = buf.subarray(i + 4);
  const tramas = [];
  while (resto.length >= 2) {
    const fin = (resto[0] & 0x80) !== 0;
    const opcode = resto[0] & 0x0f;
    let n = resto[1] & 0x7f;
    let o = 2;
    if (n === 126) { n = resto.readUInt16BE(2); o = 4; }
    else if (n === 127) { n = Number(resto.readBigUInt64BE(2)); o = 10; }
    if (resto.length < o + n) break;
    tramas.push({ fin, opcode, carga: resto.subarray(o, o + n) });
    resto = resto.subarray(o + n);
  }
  return { cabeceras, tramas };
}

export const esperar = (ms) => new Promise((r) => setTimeout(r, ms));

// ───────── Cliente del puente (a bajo nivel, para las pruebas) ─────────
import WebSocket from 'ws';

export class Control {
  constructor(ws) {
    this.ws = ws; this.n = 1; this.pendientes = new Map(); this.eventos = []; this.esperas = [];
    ws.on('message', (d, bin) => {
      if (bin) return;
      const m = JSON.parse(d.toString());
      if (m.evento) {
        this.eventos.push(m);
        this.esperas = this.esperas.filter((e) => { if (e.f(m)) { e.res(m); return false; } return true; });
      } else if (this.pendientes.has(m.id)) {
        this.pendientes.get(m.id)(m); this.pendientes.delete(m.id);
      }
    });
  }
  static abrir(puerto, origen, { host } = {}) {
    return new Promise((res, rej) => {
      const ws = new WebSocket(`ws://127.0.0.1:${puerto}/control`, { origin: origen, headers: host ? { Host: host } : {} });
      ws.once('open', () => res(new Control(ws)));
      ws.once('error', rej);
      ws.once('unexpected-response', (_q, r) => rej(Object.assign(new Error('http ' + r.statusCode), { estado: r.statusCode })));
    });
  }
  pedir(op, args = {}, { plazoMs = 20000 } = {}) {
    return new Promise((res, rej) => {
      const id = this.n++;
      const t = setTimeout(() => { this.pendientes.delete(id); rej(new Error(`sin respuesta a ${op}`)); }, plazoMs);
      this.pendientes.set(id, (m) => { clearTimeout(t); res(m); });
      this.ws.send(JSON.stringify({ id, op, ...args }));
    });
  }
  /** Espera un evento que cumpla `f` (mira también los que ya llegaron). */
  evento(f, plazoMs = 20000) {
    const ya = this.eventos.find(f);
    if (ya) return Promise.resolve(ya);
    return new Promise((res, rej) => {
      const t = setTimeout(() => rej(new Error('no llegó el evento esperado')), plazoMs);
      this.esperas.push({ f, res: (m) => { clearTimeout(t); res(m); } });
    });
  }
  cerrar() { this.ws.close(); }
}

export function abrirAudio(puerto, instancia, ticket, origen) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(`ws://127.0.0.1:${puerto}/audio/${instancia}?t=${ticket}`, { origin: origen });
    ws.binaryType = 'nodebuffer';
    ws.once('open', () => res(new Audio(ws)));
    ws.once('error', rej);
    ws.once('unexpected-response', (_q, r) => rej(Object.assign(new Error('http ' + r.statusCode), { estado: r.statusCode })));
  });
}

export class Audio {
  constructor(ws) {
    this.ws = ws; this.cola = []; this.esperas = []; this.cierre = null; this.sec = 1;
    ws.on('message', (d) => { const e = this.esperas.shift(); if (e) e(d); else this.cola.push(d); });
    this.cerrado = new Promise((r) => ws.once('close', (codigo) => { this.cierre = codigo; r(codigo); }));
  }
  recibir(plazoMs = 5000) {
    if (this.cola.length) return Promise.resolve(this.cola.shift());
    return new Promise((res, rej) => {
      const t = setTimeout(() => rej(new Error('el audio no contestó')), plazoMs);
      this.esperas.push((d) => { clearTimeout(t); res(d); });
    });
  }
  /** Manda un bloque y devuelve la respuesta ya interpretada. `canales`: array de Float32Array (o vacío). */
  async bloque({ muestras, canales = [], midi = [], banderas = 0, sec } = {}) {
    const s = sec ?? this.sec++;
    const cab = Buffer.alloc(16);
    cab.writeUInt32LE(s, 0); cab.writeUInt16LE(canales.length, 4); cab.writeUInt16LE(muestras, 6);
    cab.writeUInt16LE(midi.length, 8); cab.writeUInt16LE(banderas, 10);
    const ev = Buffer.alloc(8 * midi.length);
    midi.forEach((e, i) => { ev.writeUInt16LE(e.desp, i * 8); ev[i * 8 + 2] = e.estado; ev[i * 8 + 3] = e.d1 ?? 0; ev[i * 8 + 4] = e.d2 ?? 0; ev[i * 8 + 5] = e.long ?? 3; });
    const au = Buffer.alloc(4 * muestras * canales.length);
    canales.forEach((c, i) => Buffer.from(c.buffer, c.byteOffset, c.byteLength).copy(au, i * muestras * 4));
    this.ws.send(Buffer.concat([cab, ev, au]));
    return interpretarRespuesta(await this.recibir());
  }
  enviarCrudo(b) { this.ws.send(b); }
  cerrar() { this.ws.close(); }
}

export function interpretarRespuesta(b) {
  const sec = b.readUInt32LE(0), canales = b.readUInt16LE(4), muestras = b.readUInt16LE(6);
  const latencia = b.readUInt32LE(8), carga = b.readUInt16LE(12), banderas = b.readUInt16LE(14);
  const audio = [];
  for (let c = 0; c < canales; c++) {
    const f = new Float32Array(muestras);
    for (let i = 0; i < muestras; i++) f[i] = b.readFloatLE(16 + (c * muestras + i) * 4);
    audio.push(f);
  }
  return { sec, canales, muestras, latencia, carga, banderas, audio, bytes: b.length };
}
