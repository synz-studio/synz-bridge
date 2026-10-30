// Prueba de humo del puente en el sistema donde se ejecuta (la usa el trabajo de macOS de puente.yml):
// arranca SYNZ Bridge sin ventana, comprueba «hola» y que el escaneo termina (en Mac, con los AU de Apple).
//   EXE_PUENTE="/ruta/SYNZ Bridge" node humo.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { arrancar, parar, Control, esperar } from './ayuda.mjs';

const EXE = process.env.EXE_PUENTE;
if (!EXE) throw new Error('falta EXE_PUENTE');
const ORIGEN = 'http://localhost:5555';
const datos = fs.mkdtempSync(path.join(os.tmpdir(), 'puente-humo-'));
const p = await arrancar(EXE, ['--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada', '--puerto', '0',
  '--datos', datos, '--permitir-origen', ORIGEN], { plazoMs: 90000 });
let fallo = null;
try {
  const c = await Control.abrir(p.puerto, ORIGEN);
  const h = await c.pedir('hola');
  console.log('hola:', JSON.stringify(h));
  if (!h.ok || h.protocolo !== 1) throw new Error('«hola» no es correcto');
  if (h.sistema !== { linux: 'linux', win32: 'windows', darwin: 'mac' }[process.platform]) throw new Error('sistema distinto');
  let f = h;
  for (let i = 0; i < 400 && f.escaneando; i++) { await esperar(300); f = await c.pedir('hola'); }
  if (f.escaneando) throw new Error('el escaneo no terminó en 2 minutos');
  const l = await c.pedir('listar');
  const formatos = [...new Set(l.plugins.map((x) => x.formato))];
  console.log(`escaneo: ${l.plugins.length} plugins, formatos: ${formatos.join(', ') || '(ninguno)'}`);
  console.log('formatos que sabe alojar:', (h.formatos || []).join(', '));
  c.cerrar();
} catch (e) { fallo = e; }
await parar(p);
if (fallo) { console.error('FALLO:', fallo.message); console.error(p.errores()); process.exit(1); }
console.log('humo OK');
