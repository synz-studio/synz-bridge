// Los sensores por OSC (PROTOCOLO.md §7): mensajes OSC de verdad por UDP y lo que llega al estudio por /sensores.
//   EXE_PUENTE="/ruta/SYNZ Bridge" node --test sensores.test.mjs
import { describe, test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import WebSocket from 'ws';
import { arrancar, parar, esperar } from './ayuda.mjs';

const EXE = process.env.EXE_PUENTE;
const A = 'http://localhost:5555';
assert.ok(EXE, 'falta EXE_PUENTE');

/** Un mensaje OSC 1.0: dirección, etiquetas de tipo y argumentos (números enteros → i, el resto → f). */
function osc(direccion, ...args) {
  const cadena = (s) => { const b = Buffer.from(s + '\0'); return Buffer.concat([b, Buffer.alloc((4 - (b.length % 4)) % 4)]); };
  const tipos = ',' + args.map((a) => (Number.isInteger(a) ? 'i' : 'f')).join('');
  const datos = args.map((a) => { const b = Buffer.alloc(4); if (Number.isInteger(a)) b.writeInt32BE(a); else b.writeFloatBE(a); return b; });
  return Buffer.concat([cadena(direccion), cadena(tipos), ...datos]);
}

function abrirSensores(puerto) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(`ws://127.0.0.1:${puerto}/sensores`, { origin: A });
    const llegado = [];
    ws.on('message', (d) => { const m = JSON.parse(d.toString()); (Array.isArray(m) ? m : [m]).forEach((x) => llegado.push(x)); });
    ws.once('open', () => res({ ws, llegado }));
    ws.once('error', rej);
  });
}

async function hasta(f, ms = 3000) {
  for (let t = 0; t < ms; t += 25) { const r = f(); if (r) return r; await esperar(25); }
  throw new Error('no llegó a tiempo');
}

describe('sensores por OSC', () => {
  let p, osc_, udp, s;
  const manda = (b) => new Promise((r) => udp.send(b, osc_, '127.0.0.1', r));

  before(async () => {
    const datos = fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-'));
    p = await arrancar('xvfb-run', ['-a', '-s', '-screen 0 1280x800x24', EXE, '--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada', '--puerto', '0', '--puerto-osc', '0', '--datos', datos,
      '--permitir-origen', A], { esperar: /^LISTO (\d+)[\s\S]*^OSC (-?\d+)/m, grupo: true });
    osc_ = Number(p.salida().match(/^OSC (-?\d+)/m)[1]);
    assert.ok(osc_ > 0, 'el OSC no se abrió');
    udp = dgram.createSocket('udp4');
    s = await abrirSensores(p.puerto);
  });
  after(async () => { s?.ws.close(); udp?.close(); await parar(p); });

  test('/synz/sensor/<id>/<salida>: alta y valor', async () => {
    await manda(osc('/synz/sensor/lab-flex/indice', 0.25));
    const alta = await hasta(() => s.llegado.find((m) => m.synz === 'sensor' && m.id === 'lab-flex'));
    assert.deepEqual(alta.salidas.map((x) => x.id), ['indice']);
    const v = await hasta(() => s.llegado.find((m) => m.id === 'lab-flex' && m.v && m.v.indice !== undefined));
    assert.ok(Math.abs(v.v.indice - 0.25) < 1e-6);
  });

  test('una salida nueva vuelve a dar de alta el sensor, con las dos', async () => {
    s.llegado.length = 0;
    await manda(osc('/synz/sensor/lab-flex/medio', 7));
    const alta = await hasta(() => s.llegado.find((m) => m.synz === 'sensor' && m.id === 'lab-flex'));
    assert.deepEqual(alta.salidas.map((x) => x.id), ['indice', 'medio']);
    assert.deepEqual(alta.salidas.map((x) => x.l), ['indi', 'medi']);
  });

  test('/synz/rango: el rango viaja en el alta', async () => {
    s.llegado.length = 0;
    await manda(osc('/synz/rango/lab-flex/medio', 0, 1023));
    const alta = await hasta(() => s.llegado.find((m) => m.synz === 'sensor' && m.id === 'lab-flex'));
    const medio = alta.salidas.find((x) => x.id === 'medio');
    assert.equal(medio.min, 0); assert.equal(medio.max, 1023);
  });

  test('/synz/sensor/<id> con varios números: s1, s2…', async () => {
    await manda(osc('/synz/sensor/acc', 0.1, 0.2, 0.3));
    const alta = await hasta(() => s.llegado.find((m) => m.synz === 'sensor' && m.id === 'acc'));
    assert.deepEqual(alta.salidas.map((x) => x.id), ['s1', 's2', 's3']);
  });

  test('otra dirección cualquiera va al sensor «osc» (TouchOSC)', async () => {
    await manda(osc('/1/fader1', 0.5));
    await manda(osc('/accxyz', 0.1, -0.2, 0.9));
    const alta = await hasta(() => { const a = s.llegado.filter((m) => m.synz === 'sensor' && m.id === 'osc'); return a.length && a[a.length - 1].salidas.length === 4 && a[a.length - 1]; });
    assert.deepEqual(alta.salidas.map((x) => x.id), ['fader1', 'accxyz-1', 'accxyz-2', 'accxyz-3']);
    assert.deepEqual(alta.salidas.map((x) => x.l), ['fad1', 'accx', 'accy', 'accz']);   // 4 letras: caben bajo sus conectores
  });

  test('una conexión nueva recibe las altas de lo que ya se conoce', async () => {
    const otra = await abrirSensores(p.puerto);
    await manda(osc('/synz/sensor/lab-flex/indice', 0.75));
    await hasta(() => otra.llegado.find((m) => m.synz === 'sensor' && m.id === 'acc'));
    otra.ws.close();
  });

  test('lo que llega muy seguido se junta (como mucho un mensaje cada ~8 ms)', async () => {
    let tramas = 0; const f = () => tramas++; s.ws.on('message', f);
    for (let i = 0; i < 200; i++) manda(osc('/synz/sensor/lab-flex/indice', i / 200));
    await esperar(400); s.ws.off('message', f);
    assert.ok(tramas > 0 && tramas < 100, 'tramas: ' + tramas);
  });

  test('la ruta /sensores pide un origen permitido', async () => {
    await assert.rejects(new Promise((res, rej) => {
      const ws = new WebSocket(`ws://127.0.0.1:${p.puerto}/sensores`, { origin: 'http://otra.example' });
      ws.once('open', () => { ws.close(); res(); }); ws.once('error', rej); ws.once('unexpected-response', (_q, r) => rej(new Error('http ' + r.statusCode)));
    }));
  });

  test('«hola» dice el puerto del OSC', async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${p.puerto}/control`, { origin: A });
    await new Promise((r) => ws.once('open', r));
    const h = await new Promise((r) => { ws.once('message', (d) => r(JSON.parse(d.toString()))); ws.send(JSON.stringify({ id: 1, op: 'hola' })); });
    ws.close();
    assert.ok(h.capacidades.includes('sensoresOsc'));
    assert.equal(h.osc, osc_);
  });
});
