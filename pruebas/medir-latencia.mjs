// Mide la latencia real del puente en este navegador y este ordenador, para varios tamaños de bloque y de colchón.
//   EXE_PUENTE="/ruta/SYNZ Bridge" VST3_DIR=/ruta/vst3-pruebas node medir-latencia.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';
import { arrancar, parar } from './ayuda.mjs';

const AQUI = path.dirname(fileURLToPath(import.meta.url));
const RAIZ = path.join(AQUI, '..', 'cliente');
const tipos = { '.js': 'text/javascript', '.html': 'text/html' };
const web = await new Promise((r) => { const s = http.createServer((q, a) => {
  const ruta = new URL(q.url, 'http://x').pathname;
  const f = ruta === '/' ? path.join(AQUI, 'pagina', 'index.html') : ruta.startsWith('/cliente/') ? path.join(RAIZ, path.basename(ruta)) : path.join(AQUI, 'pagina', path.basename(ruta));
  if (!fs.existsSync(f)) { a.writeHead(404); return a.end(); }
  a.writeHead(200, { 'Content-Type': tipos[path.extname(f)] ?? 'text/plain' }); a.end(fs.readFileSync(f));
}).listen(0, () => r(s)); });
const ORIGEN = `http://localhost:${web.address().port}`;
const puente = await arrancar('xvfb-run', ['-a', '-s', '-screen 0 1280x800x24', process.env.EXE_PUENTE, '--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada', '--puerto', '0',
  '--datos', fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-')), '--permitir-origen', ORIGEN, '--carpeta-plugins', process.env.VST3_DIR], { grupo: true, plazoMs: 60000 });
await new Promise((r) => setTimeout(r, 6000));
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM ?? '/opt/pw-browsers/chromium', args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'] });
console.log('bloque colchon | retardo medido (muestras) | menos el plugin | cortes | descartes | diff env-rec | rtt ms | baseLatency ms');
for (const [bloque, colchon] of [[128, 128], [128, 256], [128, 384], [128, 512], [256, 256], [256, 512], [512, 512], [512, 1024]]) {
  const ctx = await browser.newContext();
  const p = await ctx.newPage();
  await p.goto(ORIGEN + '/');
  await p.waitForFunction(() => window.listo === true);
  const m = await p.evaluate(([u, b, c]) => montar(u, 'SynzPruebaEfecto', { bloque: b, colchon: c }), [`ws://127.0.0.1:${puente.puerto}`, bloque, colchon]);
  await p.evaluate(() => grabar());
  const frames = await p.evaluate(async () => {
    const { ctx, inst } = S;
    const buf = ctx.createBuffer(1, 64, ctx.sampleRate); buf.getChannelData(0)[0] = 1;
    const out = [];
    for (const t of [0.4, 0.9]) { const f = ctx.createBufferSource(); f.buffer = buf; f.connect(inst.in); const i = ctx.currentTime + t; f.start(i); out.push(Math.round(i * ctx.sampleRate)); }
    await esperar(2500);
    return out;
  });
  const g = await p.evaluate(() => terminarGrabacion());
  const x = g.canal0; const picos = [];
  for (let i = 0; i < x.length; i++) if (Math.abs(x[i]) > 0.5) { picos.push(g.inicio + i); i += 4000; }
  const e = await p.evaluate(() => ({ ...S.inst.estado, base: S.ctx.baseLatency * 1000 }));
  const ret = picos.map((pk, k) => pk - frames[k]);
  console.log(`${String(bloque).padStart(5)} ${String(colchon).padStart(7)} | ${ret.join(', ').padEnd(24)} | ${ret.map((r) => r - 64).join(', ').padEnd(15)} | ${String(e.cortes).padStart(6)} | ${String(e.descartes).padStart(9)} | ${String(e.enviados - e.recibidos).padStart(12)} | ${e.rttMs.toFixed(2).padStart(6)} | ${e.base.toFixed(2)}`);
  await ctx.close();
}
await browser.close(); await parar(puente); web.close(); process.exit(0);
