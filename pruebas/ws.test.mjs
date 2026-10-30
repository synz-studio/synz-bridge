// Pruebas del servidor WebSocket propio (programa ws-eco). Hace falta haber compilado el destino `ws-eco`:
//   EXE_WS_ECO=/ruta/a/ws-eco node --test ws.test.mjs
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import WebSocket from 'ws';
import { arrancar, parar, crudo, peticion, trama, leerRespuesta, esperar } from './ayuda.mjs';

const EXE = process.env.EXE_WS_ECO;
const ORIGEN = 'http://localhost:5555';
let srv;
let P;

before(async () => {
  assert.ok(EXE, 'falta EXE_WS_ECO');
  srv = await arrancar(EXE, ['--puerto', '0', '--origen', ORIGEN, '--carga-max', '200000', '--max-conexiones', '12']);
  P = srv.puerto;
});
after(async () => { await parar(srv); });

const abrir = (ruta = '/eco', opciones = {}) =>
  new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${P}${ruta}`, { origin: ORIGEN, ...opciones });
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, res) => reject(Object.assign(new Error('http ' + res.statusCode), { estado: res.statusCode })));
  });

test('GET / responde 200 con el texto fijo y sin CORS', async () => {
  const r = await fetch(`http://127.0.0.1:${P}/`);
  assert.equal(r.status, 200);
  assert.equal(await r.text(), 'synz-puente-eco 1');
  assert.equal(r.headers.get('access-control-allow-origin'), null);
});

test('texto y binario hacen eco, también uno grande', async () => {
  const ws = await abrir();
  const recibido = (tipo) => new Promise((res) => ws.once('message', (d, bin) => res({ d, bin })));
  let p = recibido(); ws.send('hola ñ 🎹'); let m = await p;
  assert.equal(m.bin, false); assert.equal(m.d.toString(), 'hola ñ 🎹');
  const grande = Buffer.alloc(150000, 7);
  p = recibido(); ws.send(grande); m = await p;
  assert.equal(m.bin, true); assert.ok(Buffer.compare(m.d, grande) === 0);
  ws.close();
});

test('ping recibe pong', async () => {
  const ws = await abrir();
  const pong = new Promise((res) => ws.once('pong', res));
  ws.ping(Buffer.from('abc'));
  const d = await pong;
  assert.equal(d.toString(), 'abc');
  ws.close();
});

test('sin Origin o con origen no permitido: 403', async () => {
  const sin = await crudo(P, peticion({ host: `127.0.0.1:${P}` }));
  assert.match(sin.toString(), /^HTTP\/1\.1 403/);
  const malo = await crudo(P, peticion({ host: `127.0.0.1:${P}`, origen: 'https://malo.example' }));
  assert.match(malo.toString(), /^HTTP\/1\.1 403/);
  const nulo = await crudo(P, peticion({ host: `127.0.0.1:${P}`, origen: 'null' }));
  assert.match(nulo.toString(), /^HTTP\/1\.1 403/);
});

test('Host ajeno (rebinding de DNS) o ausente: 403', async () => {
  const otro = await crudo(P, peticion({ host: 'atacante.example:' + P, origen: ORIGEN }));
  assert.match(otro.toString(), /^HTTP\/1\.1 403/);
  const otroPuerto = await crudo(P, peticion({ host: '127.0.0.1:9', origen: ORIGEN }));
  assert.match(otroPuerto.toString(), /^HTTP\/1\.1 403/);
  const ninguno = await crudo(P, peticion({ host: null, origen: ORIGEN }));
  assert.match(ninguno.toString(), /^HTTP\/1\.1 403/);
  // «localhost» sí vale
  const bien = await crudo(P, peticion({ host: `localhost:${P}`, origen: ORIGEN }));
  assert.match(bien.toString(), /^HTTP\/1\.1 101/);
});

test('ruta inexistente: 404; clave mala: 400; versión distinta: 426', async () => {
  const h = `127.0.0.1:${P}`;
  assert.match((await crudo(P, peticion({ host: h, origen: ORIGEN, ruta: '/otra' }))).toString(), /^HTTP\/1\.1 404/);
  assert.match((await crudo(P, peticion({ host: h, origen: ORIGEN, clave: 'corta' }))).toString(), /^HTTP\/1\.1 400/);
  const v = (await crudo(P, peticion({ host: h, origen: ORIGEN, version: '8' }))).toString();
  assert.match(v, /^HTTP\/1\.1 426/);
  assert.match(v, /Sec-WebSocket-Version: 13/i);
});

test('cabeceras repetidas y mal formadas: 400', async () => {
  const h = `127.0.0.1:${P}`;
  const doble = peticion({ host: h, origen: ORIGEN, extra: `Origin: ${ORIGEN}\r\n` });
  assert.match((await crudo(P, doble)).toString(), /^HTTP\/1\.1 400/);
  const rota = `GET /eco HTTP/1.1\r\nHost: ${h}\r\nsinDosPuntos\r\n\r\n`;
  assert.match((await crudo(P, rota)).toString(), /^HTTP\/1\.1 400/);
  assert.match((await crudo(P, 'BASURA\r\n\r\n')).toString(), /^HTTP\/1\.1 400/);
});

test('cabeceras demasiado grandes: 431', async () => {
  const h = `127.0.0.1:${P}`;
  const grande = peticion({ host: h, origen: ORIGEN, extra: 'X-Relleno: ' + 'a'.repeat(9000) + '\r\n' });
  assert.match((await crudo(P, grande)).toString(), /^HTTP\/1\.1 431/);
});

async function conectarCrudo() {
  const s = await new Promise((res) => {
    import('node:net').then(({ default: net }) => {
      const c = net.connect(P, '127.0.0.1', () => res(c));
    });
  });
  const trozos = [];
  s.on('data', (d) => trozos.push(d));
  s.write(peticion({ host: `127.0.0.1:${P}`, origen: ORIGEN }));
  await esperar(150);
  assert.match(Buffer.concat(trozos).toString(), /^HTTP\/1\.1 101/);
  const cierre = new Promise((res) => s.on('close', res));
  return { s, cierre, recibido: () => Buffer.concat(trozos) };
}

const cabecerasLen = (b) => b.indexOf('\r\n\r\n') + 4;
function tramasDespuesDelHandshake(b) {
  return leerRespuesta(b).tramas;
}
const codigoDeCierre = (tramas) => {
  const t = tramas.find((x) => x.opcode === 8);
  return t ? t.carga.readUInt16BE(0) : null;
};

test('trama del cliente sin máscara: cierra con 1002', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 1, carga: Buffer.from('hola'), enmascarar: false }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1002);
});

test('bits RSV puestos: 1002', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 1, carga: Buffer.from('hola'), rsv: 4 }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1002);
});

test('mensaje más grande que el límite: 1009, sin reservar la memoria declarada', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 2, carga: Buffer.alloc(0), largoDeclarado: 2 ** 40 }).subarray(0, 14));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1009);
});

test('control fragmentado o con más de 125 bytes: 1002', async () => {
  let c = await conectarCrudo();
  c.s.write(trama({ opcode: 9, fin: false, carga: Buffer.from('x') }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1002);
  c = await conectarCrudo();
  c.s.write(trama({ opcode: 9, carga: Buffer.alloc(126) }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1002);
});

test('texto con UTF-8 no válido: 1007', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 1, carga: Buffer.from([0xff, 0xfe, 0x41]) }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1007);
});

test('continuación sin mensaje empezado, o mensaje nuevo en mitad de otro: 1002', async () => {
  let c = await conectarCrudo();
  c.s.write(trama({ opcode: 0, carga: Buffer.from('x') }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1002);
  c = await conectarCrudo();
  c.s.write(trama({ opcode: 1, fin: false, carga: Buffer.from('a') }));
  c.s.write(trama({ opcode: 1, carga: Buffer.from('b') }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1002);
});

test('mensaje fragmentado con un ping en medio se reensambla', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 1, fin: false, carga: Buffer.from('uno ') }));
  c.s.write(trama({ opcode: 9, carga: Buffer.from('p') }));
  c.s.write(trama({ opcode: 0, fin: false, carga: Buffer.from('dos ') }));
  c.s.write(trama({ opcode: 0, fin: true, carga: Buffer.from('tres') }));
  await esperar(300);
  const t = tramasDespuesDelHandshake(c.recibido());
  assert.ok(t.some((x) => x.opcode === 10 && x.carga.toString() === 'p'), 'falta el pong');
  assert.ok(t.some((x) => x.opcode === 1 && x.carga.toString() === 'uno dos tres'), 'falta el mensaje reensamblado');
  c.s.write(trama({ opcode: 8, carga: Buffer.from([0x03, 0xe8]) }));
  await c.cierre;
});

test('el cierre ordenado se contesta con el mismo código', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 8, carga: Buffer.from([0x03, 0xe9]) }));
  await c.cierre;
  assert.equal(codigoDeCierre(tramasDespuesDelHandshake(c.recibido())), 1001);
});

test('un cliente que deja una trama a medias no ata al servidor', async () => {
  const c = await conectarCrudo();
  c.s.write(trama({ opcode: 1, carga: Buffer.from('hola mundo') }).subarray(0, 6));
  c.s.destroy();
  await c.cierre;
  const ws = await abrir();            // el servidor sigue atendiendo
  const p = new Promise((res) => ws.once('message', res));
  ws.send('sigo');
  assert.equal((await p).toString(), 'sigo');
  ws.close();
});

test('ruido aleatorio: el servidor aguanta y sigue respondiendo', async () => {
  const crypto = await import('node:crypto');
  for (let i = 0; i < 40; i++)
    await crudo(P, crypto.randomBytes(1 + (i * 37) % 600), { ms: 60 });
  const c = await conectarCrudo();
  const basura = Buffer.concat([...Array(30)].map(() => trama({ opcode: crypto.randomInt(0, 16), carga: crypto.randomBytes(crypto.randomInt(0, 40)), fin: Math.random() > 0.5 })));
  c.s.write(basura);
  await Promise.race([c.cierre, esperar(800)]);
  c.s.destroy();
  const ws = await abrir();
  ws.close();
});

test('no deja pasar de las conexiones máximas', async () => {
  const abiertas = [];
  for (let i = 0; i < 12; i++) abiertas.push(await abrir().catch(() => null));
  const vivas = abiertas.filter(Boolean);
  const otra = await abrir().then((w) => { w.close(); return true; }, () => false);
  assert.equal(otra, false, 'la conexión número 13 debería rechazarse');
  vivas.forEach((w) => w.close());
  await esperar(500);
  const ws = await abrir();            // al cerrarse, vuelve a haber hueco
  ws.close();
});

test('la ruta muda lee sin contestar; cerrar desde el servidor funciona', async () => {
  const ws = await abrir('/mudo');
  ws.send('a'); ws.send('b');
  await esperar(150);
  assert.equal(ws.readyState, WebSocket.OPEN);
  ws.close();
  const w2 = await abrir();
  const cerrada = new Promise((res) => w2.once('close', (codigo) => res(codigo)));
  w2.send('cerrar');
  assert.equal(await cerrada, 1000);
});

test('al parar el servidor con conexiones abiertas, termina enseguida', async () => {
  const otro = await arrancar(EXE, ['--puerto', '0', '--origen', ORIGEN]);
  const ws = await new Promise((res, rej) => {
    const w = new WebSocket(`ws://127.0.0.1:${otro.puerto}/eco`, { origin: ORIGEN });
    w.once('open', () => res(w)); w.once('error', rej);
  });
  const t0 = Date.now();
  await parar(otro);
  assert.ok(Date.now() - t0 < 3500, 'tardó demasiado en cerrar: ' + (Date.now() - t0) + ' ms');
  ws.terminate();
});
