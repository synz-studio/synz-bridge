// Pruebas del cliente del navegador, con Chromium de verdad contra el puente real y plugins VST3 reales.
//   EXE_PUENTE="/ruta/SYNZ Bridge" VST3_DIR=/ruta/vst3-pruebas CHROMIUM=/ruta/chromium node --test cliente.test.mjs
import { describe, test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import http from 'node:http';
import { chromium } from 'playwright-core';
import { arrancar, parar } from './ayuda.mjs';

const EXE = process.env.EXE_PUENTE;
const VST3 = process.env.VST3_DIR;
const CHROMIUM = process.env.CHROMIUM ?? '/opt/pw-browsers/chromium';
const AQUI = path.dirname(fileURLToPath(import.meta.url));
const RAIZ_CLIENTE = path.join(AQUI, '..', 'cliente');
let ORIGEN;                      // se fija al arrancar el servidor web de las pruebas
assert.ok(EXE && VST3, 'faltan EXE_PUENTE y VST3_DIR');

const tipos = { '.js': 'text/javascript', '.html': 'text/html', '.mjs': 'text/javascript' };

/** Sirve la página de pruebas y el cliente desde disco, como haría el estudio con sus archivos. */
function servirWeb() {
  const srv = http.createServer((req, res) => {
    const ruta = new URL(req.url, 'http://x').pathname;
    let archivo;
    if (ruta === '/' || ruta === '/index.html') archivo = path.join(AQUI, 'pagina', 'index.html');
    else if (ruta.startsWith('/cliente/')) archivo = path.join(RAIZ_CLIENTE, path.basename(ruta));
    else if (ruta.startsWith('/pagina/')) archivo = path.join(AQUI, 'pagina', path.basename(ruta));
    if (!archivo || !fs.existsSync(archivo)) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'Content-Type': tipos[path.extname(archivo)] ?? 'application/octet-stream' });
    res.end(fs.readFileSync(archivo));
  });
  return new Promise((r) => srv.listen(0, () => r(srv)));
}

async function arrancarPuente() {
  const datos = fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-'));
  const proc = await arrancar('xvfb-run', ['-a', '-s', '-screen 0 1280x800x24', EXE, '--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada',
    '--puerto', '0', '--datos', datos, '--permitir-origen', ORIGEN, '--carpeta-plugins', VST3], { grupo: true, plazoMs: 60000 });
  return proc;
}

async function pagina(browser) {
  const ctx = await browser.newContext();
  const p = await ctx.newPage();
  const errores = [];
  p.on('pageerror', (e) => errores.push(e.message));
  p.on('console', (m) => { if (m.type() === 'error') errores.push(m.text()); });
  await p.goto(ORIGEN + '/');
  await p.waitForFunction(() => window.listo === true);
  return { p, errores, cerrar: () => ctx.close() };
}

/**
 * Este entorno (contenedor compartido, Chromium sin tarjeta de sonido) tiene paradas esporádicas del hilo de audio del
 * navegador, que se ven como un corte aunque el puente conteste en 1-5 ms. Las pruebas que miden tiempos exactos se
 * repiten hasta cinco veces: se repite la medición, nunca se relaja lo que se exige en ella.
 */
const conReintentos = (cuerpo, veces = 5) => async (t) => {
  for (let k = 1; ; k++) {
    try { return await cuerpo(t); } catch (e) {
      if (k >= veces) throw e;
      console.log(`  (intento ${k} fallido: ${String(e.message).split('\n')[0]}; se repite)`);
    }
  }
};

const primerNoCero = (a, umbral = 0.01) => a.findIndex((x) => Math.abs(x) > umbral);

describe('cliente del navegador', () => {
  let puente, browser, url, web;

  before(async () => {
    web = await servirWeb();
    ORIGEN = `http://localhost:${web.address().port}`;
    puente = await arrancar('xvfb-run', ['-a', '-s', '-screen 0 1280x800x24', EXE, '--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada',
      '--puerto', '0', '--datos', fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-')), '--permitir-origen', ORIGEN, '--carpeta-plugins', VST3], { grupo: true, plazoMs: 60000 });
    url = `ws://127.0.0.1:${puente.puerto}`;
    browser = await chromium.launch({ executablePath: CHROMIUM, args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'] });
    // Espera a que el puente termine de escanear
    const w = await import('ws');
    for (let i = 0; i < 100; i++) {
      const n = await new Promise((res) => {
        const s = new w.default(url + '/control', { origin: ORIGEN });
        s.on('open', () => s.send(JSON.stringify({ id: 1, op: 'hola' })));
        s.on('message', (d) => { const m = JSON.parse(d); s.close(); res(m.escaneando ? 0 : m.plugins); });
        s.on('error', () => res(0));
      });
      if (n >= 3) break;
      await new Promise((r) => setTimeout(r, 200));
    }
  });
  after(async () => { await browser?.close(); await parar(puente); web?.close(); web?.closeAllConnections?.(); });

  test('sin puente, conectar rechaza con un error claro', async () => {
    const { p, cerrar } = await pagina(browser);
    const r = await p.evaluate(async () => {
      try { await PuenteCliente.conectar({ url: 'ws://127.0.0.1:9', plazoMs: 2000 }); return 'conectó'; }
      catch (e) { return e instanceof ErrorPuente ? e.codigo : 'otro: ' + e; }
    });
    assert.equal(r, 'no_disponible');
    await cerrar();
  });

  test('instrumento: dos notas separadas por 4800 muestras suenan separadas por 4800 muestras exactas', conReintentos(async () => {
    const { p, errores, cerrar } = await pagina(browser);
    const m = await p.evaluate((u) => montar(u, 'SynzPruebaSinte'), url);
    assert.equal(m.info.instrumento, true);
    await p.evaluate(() => grabar());
    const { F1, F2 } = await p.evaluate(async () => {
      const { ctx, inst } = S;
      const t0 = ctx.currentTime + 0.4;
      const F1 = Math.round(t0 * ctx.sampleRate);
      inst.noteOn(69, 1.0, t0);
      inst.noteOff(69, t0 + 0.02);
      inst.noteOn(69, 1.0, t0 + 4800 / ctx.sampleRate);
      inst.noteOff(69, t0 + 4800 / ctx.sampleRate + 0.02);
      await esperar(1500);
      return { F1, F2: F1 + 4800 };
    });
    const g = await p.evaluate(() => terminarGrabacion());
    const x = g.canal0;
    const o1 = primerNoCero(x);
    assert.ok(o1 > 0, 'sonó la primera nota');
    // Segunda nota: primera muestra distinta de cero tras el silencio de entre medias
    const finNota1 = x.findIndex((v, i) => i > o1 + 100 && Math.abs(v) < 1e-6);
    const o2 = finNota1 + x.slice(finNota1).findIndex((v) => Math.abs(v) > 0.01);
    assert.equal(o2 - o1, 4800, `separación entre notas: ${o2 - o1}`);
    const latenciaMedida = g.inicio + o1 - F1;
    console.log(`  latencia medida: ${latenciaMedida} muestras (${(latenciaMedida / m.sr * 1000).toFixed(2)} ms); la que declara el cliente: ${m.latencia} (${(m.latencia / m.sr * 1000).toFixed(2)} ms)`);
    assert.ok(Math.abs(latenciaMedida - m.latencia) <= 256, 'la latencia declarada se parece a la medida');
    assert.ok(Math.abs(x[o1] - 0.5) < 0.01, 'la nota arranca con su amplitud completa en la primera muestra');
    assert.deepEqual(errores, []);
    await p.evaluate(() => S.inst.dispose());
    await cerrar();
  }));

  test('efecto: el retardo es el mismo se meta el impulso donde se meta (sin deriva) y se parece al declarado', async () => {
    const { p, errores, cerrar } = await pagina(browser);
    const m = await p.evaluate((u) => montar(u, 'SynzPruebaEfecto'), url);
    assert.equal(m.info.instrumento, false);
    await p.evaluate(() => grabar());
    const impulsos = await p.evaluate(async () => {
      const { ctx, inst } = S;
      const buf = ctx.createBuffer(1, 64, ctx.sampleRate);
      buf.getChannelData(0)[0] = 1;
      const tiempos = [0.30, 0.60 + 37 / ctx.sampleRate, 0.90 + 91 / ctx.sampleRate];   // alineaciones distintas dentro del bloque
      const frames = [];
      for (const t of tiempos) {
        const f = ctx.createBufferSource();
        f.buffer = buf;
        f.connect(inst.in);
        const inicio = ctx.currentTime + t;
        f.start(inicio);
        frames.push(Math.round(inicio * ctx.sampleRate));
      }
      await esperar(1700);
      return frames;
    });
    const g = await p.evaluate(() => terminarGrabacion());
    const x = g.canal0;
    const picos = [];
    let i = 0;
    while (i < x.length) {
      if (Math.abs(x[i]) > 0.5) { picos.push(g.inicio + i); i += 2000; } else i++;
    }
    assert.equal(picos.length, 3, 'tres impulsos a la salida');
    const retardos = picos.map((pk, k) => pk - impulsos[k]);
    console.log(`  retardos medidos: ${retardos.join(', ')} muestras (declarado: ${m.latencia}, conducto medido: ${m.conducto}; el plugin pone 64)`);
    assert.ok(retardos.every((r) => r === retardos[0]), 'el retardo es constante: ' + retardos);
    assert.ok(Math.abs(retardos[0] - m.latencia) <= 8, `la latencia declarada (${m.latencia}) no coincide con la medida (${retardos[0]})`);
    assert.deepEqual(errores, []);
    await p.evaluate(() => S.inst.dispose());
    await cerrar();
  });

  test('con el hilo principal bloqueado 500 ms el audio no se corta (el worklet y el Worker no dependen de él)', conReintentos(async () => {
    const { p, errores, cerrar } = await pagina(browser);
    await p.evaluate((u) => montar(u, 'SynzPruebaEfecto'), url);
    await p.evaluate(() => {
      const { ctx, inst } = S;
      const osc = ctx.createOscillator();
      osc.frequency.value = 220;
      osc.connect(inst.in);
      osc.start();
      grabar();
    });
    await p.evaluate(() => esperar(1500));                 // se establece
    const antes = await p.evaluate(() => ({ ...S.inst.estado }));
    await p.evaluate(() => { const fin = performance.now() + 500; while (performance.now() < fin) { /* ocupado */ } });
    await p.evaluate(() => esperar(1500));
    const despues = await p.evaluate(() => ({ ...S.inst.estado }));
    const g = await p.evaluate(() => terminarGrabacion());
    const x = g.canal0;
    // Desde que suena, no puede haber tramos de silencio exacto (un seno de 220 Hz no vale cero muestras seguidas).
    const inicio = primerNoCero(x, 0.001);
    let ceros = 0, maxCeros = 0;
    for (let i = inicio; i < x.length; i++) { if (x[i] === 0) { ceros++; maxCeros = Math.max(maxCeros, ceros); } else ceros = 0; }
    console.log(`  cortes: ${despues.cortes} (antes ${antes.cortes}), perdidos: ${despues.perdidos}, descartes: ${despues.descartes}, tramo de ceros más largo: ${maxCeros}, ida y vuelta: ${despues.rttMs.toFixed(2)} ms (máx ${despues.rttMaxMs.toFixed(2)})`);
    assert.equal(despues.cortes, antes.cortes, 'no hubo cortes durante el bloqueo');
    assert.equal(despues.perdidos, 0);
    assert.ok(maxCeros < 4, 'sin huecos de silencio: ' + maxCeros);
    assert.deepEqual(errores, []);
    await p.evaluate(() => S.inst.dispose());
    await cerrar();
  }));

  test('notas rápidas durante cuatro segundos: sin bloques perdidos y con pocos cortes (el colchón se adapta)', async () => {
    const { p, errores, cerrar } = await pagina(browser);
    await p.evaluate((u) => montar(u, 'SynzPruebaSinte'), url);
    await p.evaluate(() => esperar(1100));          // que las estadísticas (una por segundo) recojan el arranque
    const arranque = await p.evaluate(() => ({ ...S.inst.estado }));
    await p.evaluate(async () => {
      const { ctx, inst } = S;
      const t0 = ctx.currentTime + 0.3;
      for (let k = 0; k < 160; k++) {
        inst.noteOn(48 + (k % 24), 0.8, t0 + k * 0.025);
        inst.noteOff(48 + (k % 24), t0 + k * 0.025 + 0.02);
      }
      await esperar(4600);
    });
    const e = await p.evaluate(() => ({ ...S.inst.estado }));
    const colchon = await p.evaluate(() => S.inst.colchon);
    console.log(`  estado: cortes ${e.cortes} (tras el arranque ${arranque.cortes}), perdidos ${e.perdidos}, descartes ${e.descartes}, colchón ${colchon}, enviados ${e.enviados}, recibidos ${e.recibidos}, carga del plugin ${e.carga / 100} %, ida y vuelta máx ${e.rttMaxMs.toFixed(1)} ms`);
    // Este entorno (contenedor compartido, Chromium sin tarjeta de sonido) tiene paradas de más de 10 ms de vez en cuando:
    // si el colchón se queda corto, sube solo y puede cortar alguna vez. Lo que no puede pasar es perder bloques ni cortar sin parar.
    assert.ok(e.cortes <= 4, `demasiados cortes: ${e.cortes}`);
    assert.equal(e.perdidos, 0);
    assert.ok(e.recibidos >= e.enviados - 16, 'el puente contesta a todo');
    assert.deepEqual(errores, []);
    await p.evaluate(() => S.inst.dispose());
    await cerrar();
  });

  test('parámetros y estado desde el navegador; el cambio hecho en el plugin llega como evento', async () => {
    const { p, errores, cerrar } = await pagina(browser);
    await p.evaluate((u) => montar(u, 'SynzPruebaEfecto'), url);
    const r = await p.evaluate(async () => {
      const { inst } = S;
      const eventos = [];
      inst.alParametro((i, v) => eventos.push([i, v]));
      const estado0 = await inst.leerEstado();
      await inst.ponerParametro(0, 0.9);
      const valores1 = await inst.leerParametros();
      await inst.ponerEstado(estado0);
      await esperar(300);
      const valores2 = await inst.leerParametros();
      return { valores1, valores2, eventos, nombres: inst.parametros.map((x) => x.nombre) };
    });
    assert.deepEqual(r.nombres, ['Ganancia', 'Romper', 'Bypass']);
    assert.ok(Math.abs(r.valores1[0] - 0.9) < 1e-3);
    assert.ok(Math.abs(r.valores2[0] - 0.5) < 1e-3);
    assert.ok(r.eventos.some(([i, v]) => i === 0 && Math.abs(v - 0.5) < 1e-3), 'llegó el evento de parámetro: ' + JSON.stringify(r.eventos));
    assert.deepEqual(errores, []);
    await p.evaluate(() => S.inst.dispose());
    await cerrar();
  });

  test('dispose suelta el plugin y el Worker; el puente sigue vivo', async () => {
    const { p, errores, cerrar } = await pagina(browser);
    await p.evaluate((u) => montar(u, 'SynzPruebaEfecto'), url);
    const r = await p.evaluate(async () => {
      const { puente, inst } = S;
      const id = inst.id;
      await inst.dispose();
      const e = await puente.pedir('destruir', { instancia: id }).then(() => 'existía', (x) => x.codigo);
      const h = await puente.pedir('hola');
      return { e, ok: h.ok, vivas: puente.instancias.size };
    });
    assert.equal(r.e, 'instancia_desconocida');
    assert.equal(r.ok, true);
    assert.equal(r.vivas, 0);
    assert.deepEqual(errores, []);
    await cerrar();
  });

  test('si el puente se cierra, la instancia avisa y la página no revienta', async () => {
    const otro = await arrancarPuente();
    const { p, errores, cerrar } = await pagina(browser);
    await p.evaluate((u) => montar(u, 'SynzPruebaSinte'), `ws://127.0.0.1:${otro.puerto}`);
    await p.evaluate(() => { window.AVISOS = []; S.inst.alCaer((motivo) => AVISOS.push(motivo)); S.puente.alDesconectar = () => AVISOS.push('desconectado'); });
    await parar(otro);
    await p.waitForFunction(() => AVISOS.length >= 2, null, { timeout: 8000 });
    const avisos = await p.evaluate(() => AVISOS);
    assert.ok(avisos.includes('desconectado'));
    assert.ok(avisos.some((a) => /conexión|audio/.test(a)), JSON.stringify(avisos));
    const reuso = await p.evaluate(async () => S.puente.pedir('hola').then(() => 'respondió', (e) => e.codigo));
    assert.equal(reuso, 'no_disponible');
    assert.deepEqual(errores.filter((e) => !/WebSocket|ERR_/.test(e)), []);
    await cerrar();
  });
});
