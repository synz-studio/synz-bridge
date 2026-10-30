// Diagnóstico en macOS cuando algo se cuelga (lo usa el trabajo macos de puente.yml).
//   EXE_WS_ECO=… EXE_PUENTE=… VST3_DIR=… node diagnostico-mac.mjs
// 1) ¿El servidor eco contesta a un mensaje ASCII? ¿y a uno con ñ y emoji?
// 2) ¿El puente crea un plugin VST3? Si no contesta, hace un «sample» del proceso para ver qué hace el hilo principal.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import WebSocket from 'ws';
import { arrancar, parar, Control, esperar } from './ayuda.mjs';

const ORIGEN = 'http://localhost:5555';
const cuenta = (t) => console.log('\n=== ' + t);

async function eco() {
  cuenta('servidor eco');
  const srv = await arrancar(process.env.EXE_WS_ECO, ['--puerto', '0', '--origen', ORIGEN], { plazoMs: 30000 });
  try {
    const ws = await new Promise((res, rej) => { const w = new WebSocket(`ws://127.0.0.1:${srv.puerto}/eco`, { origin: ORIGEN }); w.once('open', () => res(w)); w.once('error', rej); });
    for (const txt of ['abc', 'ñ', '🎹', 'hola ñ 🎹']) {
      const r = await new Promise((res) => { const t = setTimeout(() => res('SIN RESPUESTA'), 3000); ws.once('message', (d) => { clearTimeout(t); res(d.toString()); }); ws.send(txt); });
      console.log(JSON.stringify(txt), '->', JSON.stringify(r));
    }
    // Mensajes binarios de tamaño creciente: ¿a partir de cuál no vuelve el eco?
    for (const n of [1000, 16000, 32000, 65000, 70000, 100000, 150000]) {
      const t0 = Date.now();
      const r = await new Promise((res) => { const t = setTimeout(() => res('SIN RESPUESTA'), 4000); ws.once('message', (d) => { clearTimeout(t); res(d.length); }); ws.send(Buffer.alloc(n, 7)); });
      console.log(`binario ${n} ->`, r, `(${Date.now() - t0} ms)`);
      if (r === 'SIN RESPUESTA') break;
    }
    ws.close();
  } catch (e) { console.log('fallo:', e.message); }
  await parar(srv);
}

async function crear() {
  cuenta('crear un plugin VST3 de prueba en el puente');
  const datos = fs.mkdtempSync(path.join(os.tmpdir(), 'puente-diag-'));
  const args = ['--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada', '--puerto', '0',
    '--datos', datos, '--permitir-origen', ORIGEN, '--carpeta-plugins', process.env.VST3_DIR];
  const p = await arrancar(process.env.EXE_PUENTE, args, { plazoMs: 60000 });
  let salida = null;
  p.hijo.on('exit', (codigo, senal) => { salida = `código=${codigo} señal=${senal}`; console.log('EL PROCESO DEL PUENTE TERMINÓ:', salida); });
  try {
    const c = await Control.abrir(p.puerto, ORIGEN);
    for (let i = 0; i < 100; i++) { const h = await c.pedir('hola'); if (!h.escaneando && h.plugins >= 1) break; await esperar(200); }
    const l = await c.pedir('listar');
    console.log('plugins:', l.plugins.map((x) => `${x.nombre}[${x.formato}]`).join(', '));
    const s = l.plugins.find((x) => x.nombre === 'SynzPruebaSinte');
    if (!s) throw new Error('no aparece SynzPruebaSinte');
    const t0 = Date.now();
    try {
      const r = await c.pedir('crear', { plugin: s.id }, { plazoMs: 15000 });
      console.log('crear ->', JSON.stringify(r).slice(0, 300), `(${Date.now() - t0} ms)`);
    } catch (e) {
      console.log('crear: ' + e.message + ' tras 15 s. Muestra del proceso:');
      try { console.log(execFileSync('sample', [String(p.hijo.pid), '3', '-mayDie'], { encoding: 'utf8', timeout: 30000 }).split('\n').slice(0, 90).join('\n')); }
      catch (e2) { console.log('sample falló:', e2.message); }
    }
    c.cerrar();
  } catch (e) { console.log('fallo:', e.message); }
  await esperar(4000);
  console.log('terminó el puente:', salida ?? 'no');
  try {
    const dir = path.join(os.homedir(), 'Library', 'Logs', 'DiagnosticReports');
    const informes = fs.readdirSync(dir).filter((f) => /synz|bridge/i.test(f)).sort();
    console.log('informes de cierre inesperado:', informes.join(', ') || '(ninguno)');
    if (informes.length) console.log(fs.readFileSync(path.join(dir, informes[informes.length - 1]), 'utf8').slice(0, 9000));
  } catch (e) { console.log('sin informes:', e.message); }
  console.log('--- salida del puente:\n' + p.salida().slice(-12000));
  console.log('--- errores del puente:\n' + p.errores().slice(-2000));
  for (const f of fs.readdirSync(datos, { recursive: true })) {
    const ruta = path.join(datos, String(f));
    if (fs.statSync(ruta).isFile() && /log|registro/i.test(String(f))) console.log('--- ' + f + ':\n' + fs.readFileSync(ruta, 'utf8').slice(-2500));
  }
  await parar(p);
}

await eco();
await crear();
