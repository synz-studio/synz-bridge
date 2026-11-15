// Las entradas de la tarjeta (puente 1.4.0, para Repertoire): Chromium de verdad contra el puente real, con la tarjeta «Prueba»
// (32 tonos, 200 Hz + 50 Hz por canal) y su reloj desviado ±300 ppm del del ordenador, como una tarjeta de verdad.
//   EXE_PUENTE="/ruta/SYNZ Bridge" CHROMIUM=/ruta/chromium node --test entradas.test.mjs
// Comprueba: que el puente lo anuncia y lo lista; que llegan los 32 canales, cada uno en su sitio (su frecuencia); que en 20 s no hay
// ni un corte ni un salto en la onda; que el retardo se queda quieto (la deriva se absorbe, no se acumula); un subconjunto de canales;
// y que la misma tarjeta no se abre dos veces y se puede volver a abrir tras cerrarla.
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
const CHROMIUM = process.env.CHROMIUM ?? '/opt/pw-browsers/chromium';
const AQUI = path.dirname(fileURLToPath(import.meta.url));
const RAIZ_CLIENTE = path.join(AQUI, '..', 'cliente');
assert.ok(EXE, 'falta EXE_PUENTE');

// El analizador corre en el hilo de audio: por canal, cruces por cero (la frecuencia) y saltos imposibles para un tono limpio.
/** Este entorno (Chromium sin tarjeta) tiene paradas sueltas del hilo de audio: como en cliente.test.mjs, la medición se repite hasta
    cinco veces, sin relajar nunca lo que se exige en ella. */
const conReintentos = (cuerpo, veces = 5) => async (t) => {
  for (let k = 1; ; k++) {
    try { return await cuerpo(t); } catch (e) {
      if (k >= veces) throw e;
      console.log(`  (intento ${k} fallido: ${String(e.message).split('\n')[0]}; se repite)`);
    }
  }
};

const ANALIZA = `
class Analiza extends AudioWorkletProcessor {
  constructor() { super(); this.mide = false; this.reinicia(); this.port.onmessage = (e) => { if (e.data === 'inicio') { this.reinicia(); this.mide = true; } else if (e.data === 'fin') { this.mide = false; this.port.postMessage({ cruces: this.cruces, saltos: this.saltos, muestras: this.n, ceros: this.ceros }); } }; }
  reinicia() { this.cruces = new Array(32).fill(0); this.saltos = new Array(32).fill(0); this.ant = new Array(32).fill(0); this.n = 0; this.ceros = 0; this.primero = true; }
  process(ent) {
    const e = ent[0]; if (!this.mide || !e || !e.length) return true;
    let silencio = true;
    for (let c = 0; c < e.length; c++) {
      const x = e[c]; let a = this.ant[c];
      for (let i = 0; i < x.length; i++) { const v = x[i]; if (v !== 0) silencio = false; if (!this.primero || i > 0) { if (a < 0 && v >= 0) this.cruces[c]++; if (Math.abs(v - a) > 0.2) this.saltos[c]++; } a = v; }
      this.ant[c] = a;
    }
    if (silencio) this.ceros++;
    this.primero = false; this.n += e[0].length; return true;
  }
}
registerProcessor('analiza', Analiza);`;

function servirWeb() {
  const srv = http.createServer((req, res) => {
    const ruta = new URL(req.url, 'http://x').pathname;
    if (ruta === '/') { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end('<!doctype html><meta charset="utf-8"><script type="module">import { PuenteCliente } from "/cliente/puente-cliente.js"; window.PuenteCliente = PuenteCliente; window.listo = true;</script>'); }
    if (ruta === '/analiza.js') { res.writeHead(200, { 'Content-Type': 'text/javascript' }); return res.end(ANALIZA); }
    const archivo = ruta.startsWith('/cliente/') ? path.join(RAIZ_CLIENTE, path.basename(ruta)) : null;
    if (!archivo || !fs.existsSync(archivo)) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'Content-Type': 'text/javascript' });
    res.end(fs.readFileSync(archivo));
  });
  return new Promise((r) => srv.listen(0, () => r(srv)));
}

for (const ppm of [300, -300]) {
  describe(`entradas de la tarjeta (prueba a ${ppm > 0 ? '+' : ''}${ppm} ppm)`, () => {
    let puente, browser, web, origen, url, p;

    before(async () => {
      web = await servirWeb();
      origen = `http://localhost:${web.address().port}`;
      puente = await arrancar('xvfb-run', ['-a', '-s', '-screen 0 1280x800x24', EXE, '--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada', '--entrada-prueba',
        '--puerto', '0', '--puerto-osc', '-1', '--datos', fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-')), '--permitir-origen', origen],
        { grupo: true, plazoMs: 60000, env: { SYNZ_PRUEBA_PPM: String(ppm) } });
      url = `ws://127.0.0.1:${puente.puerto}`;
      browser = await chromium.launch({ executablePath: CHROMIUM, args: ['--autoplay-policy=no-user-gesture-required', '--no-sandbox'] });
      p = await (await browser.newContext()).newPage();
      p.on('pageerror', (e) => console.log('error de la página:', e.message));
      await p.goto(origen + '/');
      await p.waitForFunction(() => window.listo === true);
    });
    after(async () => { await browser?.close(); await parar(puente); web?.close(); });

    test('el puente lo anuncia y lista la tarjeta de prueba', async () => {
      const r = await p.evaluate(async (u) => {
        const c = await PuenteCliente.conectar({ url: u });
        const hola = await c.pedir('hola', { protocolo: 1 });
        const d = await c.dispositivos();
        c.cerrar();
        return { caps: hola.capacidades, version: hola.puente, d };
      }, url);
      assert.ok(r.caps.includes('entradas'), 'falta la capacidad «entradas»');
      assert.equal(r.version, '1.4.0');
      const prueba = r.d.find((x) => x.tipo === 'Prueba');
      assert.ok(prueba, 'no sale la tarjeta de prueba');
      assert.equal(prueba.entradas, 32);
    });

    test('32 canales en su sitio, sin cortes, y el retardo quieto durante 20 s', conReintentos(async () => {
      const r = await p.evaluate(async (u) => {
        const ctx = new AudioContext({ sampleRate: 48000 });
        await ctx.resume();
        await ctx.audioWorklet.addModule('/analiza.js');
        const c = await PuenteCliente.conectar({ url: u });
        const inst = await c.abrirEntrada(ctx, 'Prueba', 'Prueba', { bloque: 128 });
        const an = new AudioWorkletNode(ctx, 'analiza', { numberOfInputs: 1, numberOfOutputs: 0, channelCount: 32, channelCountMode: 'explicit', channelInterpretation: 'discrete' });
        inst.out.connect(an);
        const espera = (ms) => new Promise((res) => setTimeout(res, ms));
        await espera(6000);                                   // arranque y que el control de la deriva se asiente
        const cortes0 = inst.estado.cortes;
        an.port.postMessage('inicio');
        const lat = [];
        for (let s = 0; s < 20; s++) { await espera(1000); lat.push(inst.latenciaPlugin); }
        const res = await new Promise((ok) => { an.port.onmessage = (e) => ok(e.data); an.port.postMessage('fin'); });
        const out = { canales: inst.canales, nombres: inst.nombresCanales, cortes: inst.estado.cortes - cortes0, banderas: inst.estado.banderas, lat, ...res, sr: ctx.sampleRate };
        await inst.dispose(); c.cerrar(); await ctx.close();
        return out;
      }, url);
      assert.equal(r.canales, 32);
      assert.equal(r.nombres.length, 32);
      const seg = r.muestras / r.sr;
      assert.ok(seg > 18, `solo se midieron ${seg.toFixed(1)} s`);
      const saltos0 = r.saltos.reduce((a, b) => a + b, 0);
      console.log(`  ${ppm} ppm: saltos ${saltos0}, bloques en silencio ${r.ceros}, cortes ${r.cortes}, retardo (muestras) ${Math.min(...r.lat)}–${Math.max(...r.lat)}`);
      assert.equal(r.cortes, 0, 'el conducto se quedó sin audio');
      for (let c = 0; c < 32; c++) {
        const f = r.cruces[c] / seg, esperada = (200 + 50 * c) * (1 + ppm * 1e-6);   // la tarjeta lenta suena un poco más grave, y al revés
        assert.ok(Math.abs(f - esperada) < 1, `canal ${c + 1}: ${f.toFixed(2)} Hz en vez de ${esperada.toFixed(2)}`);
      }
      const saltos = r.saltos.reduce((a, b) => a + b, 0);
      assert.equal(saltos, 0, 'hay saltos en la onda (cortes o costuras)');
      assert.equal(r.ceros, 0, 'hubo bloques en silencio');
      assert.equal(r.cortes, 0, 'el conducto se quedó sin audio');
      assert.equal(r.banderas & 4, 0, 'el puente dice que no tenía audio de la tarjeta');
      // La deriva acumula 300 ppm × 20 s = 6 ms (288 muestras) si no se corrige: el retardo no puede moverse ni la mitad de eso.
      const primero = r.lat.slice(0, 5).reduce((a, b) => a + b, 0) / 5, ultimo = r.lat.slice(-5).reduce((a, b) => a + b, 0) / 5;
      assert.ok(Math.abs(ultimo - primero) < 96, `el retardo se mueve ${(ultimo - primero).toFixed(0)} muestras en 20 s`);
    }));

    test('un subconjunto de canales, en el orden de la tarjeta', async () => {
      const r = await p.evaluate(async (u) => {
        const ctx = new AudioContext({ sampleRate: 48000 });
        await ctx.resume();
        await ctx.audioWorklet.addModule('/analiza.js');
        const c = await PuenteCliente.conectar({ url: u });
        const inst = await c.abrirEntrada(ctx, 'Prueba', 'Prueba', { canales: [7, 3] });
        const an = new AudioWorkletNode(ctx, 'analiza', { numberOfInputs: 1, numberOfOutputs: 0, channelCount: 2, channelCountMode: 'explicit', channelInterpretation: 'discrete' });
        inst.out.connect(an);
        await new Promise((ok) => setTimeout(ok, 2000));
        an.port.postMessage('inicio');
        await new Promise((ok) => setTimeout(ok, 4000));
        const res = await new Promise((ok) => { an.port.onmessage = (e) => ok(e.data); an.port.postMessage('fin'); });
        const out = { canales: inst.canales, nombres: inst.nombresCanales, f: res.cruces.slice(0, 2).map((x) => x / (res.muestras / ctx.sampleRate)) };
        await inst.dispose(); c.cerrar(); await ctx.close();
        return out;
      }, url);
      assert.equal(r.canales, 2);
      assert.deepEqual(r.nombres, ['Prueba 4', 'Prueba 8']);
      assert.ok(Math.abs(r.f[0] - 350) < 1.5 && Math.abs(r.f[1] - 550) < 1.5, `frecuencias ${r.f.map((x) => x.toFixed(1))}`);
    });

    test('la misma tarjeta no se abre dos veces, y se vuelve a abrir tras cerrarla', async () => {
      const r = await p.evaluate(async (u) => {
        const ctx = new AudioContext();
        const c = await PuenteCliente.conectar({ url: u });
        const a = await c.abrirEntrada(ctx, 'Prueba', 'Prueba', { canales: [0] });
        let dos = null;
        try { await c.abrirEntrada(ctx, 'Prueba', 'Prueba', { canales: [1] }); } catch (e) { dos = e.codigo; }
        await a.dispose();
        const b = await c.abrirEntrada(ctx, 'Prueba', 'Prueba', { canales: [1] });
        const ok = b.canales === 1;
        await b.dispose(); c.cerrar(); await ctx.close();
        return { dos, ok };
      }, url);
      assert.equal(r.dos, 'limite');
      assert.ok(r.ok);
    });
  });
}
