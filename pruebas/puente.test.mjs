// Pruebas de extremo a extremo del puente con plugins VST3 reales (los de pruebas/plugins).
//   EXE_PUENTE="/ruta/SYNZ Bridge" VST3_DIR=/ruta/vst3-pruebas node --test puente.test.mjs
// Necesita un servidor X (xvfb-run) porque las ventanas de los plugins se abren de verdad.
import { describe, test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { arrancar, parar, sinXvfb, Control, abrirAudio, esperar } from './ayuda.mjs';

const EXE = process.env.EXE_PUENTE;
const VST3 = process.env.VST3_DIR;
const A = 'http://localhost:5555';
const B = 'http://localhost:6666';

assert.ok(EXE && VST3, 'faltan EXE_PUENTE y VST3_DIR');

async function arrancarPuente({ env = {}, extra = [], datos } = {}) {
  const carpeta = datos ?? fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-'));
  const proc = await arrancar('xvfb-run', ['-a', '-s', '-screen 0 1280x800x24', EXE,
    '--sin-ventana', '--sin-preguntar', '--cerrar-con-entrada', '--puerto', '0', '--datos', carpeta,
    '--permitir-origen', A, '--permitir-origen', B, '--carpeta-plugins', VST3, ...extra], { env, grupo: true, plazoMs: 60000 });
  return { proc, puerto: proc.puerto, datos: carpeta };
}

async function esperarEscaneo(c, minimo = 1) {
  for (let i = 0; i < 200; i++) {
    const h = await c.pedir('hola');
    if (!h.escaneando && h.plugins >= minimo) return h;
    await esperar(150);
  }
  throw new Error('el escaneo no terminó');
}

const senoEsperado = (n, ganancia = 0.5, fs_ = 48000, hz = 440) => ganancia * Math.sin(2 * Math.PI * (0.25 + (hz * n) / fs_));

describe('plugins reales', () => {
  let p, ctl, plugins;

  before(async () => {
    p = await arrancarPuente();
    ctl = await Control.abrir(p.puerto, A);
    await esperarEscaneo(ctl, 3);
    plugins = (await ctl.pedir('listar')).plugins;
  });
  after(async () => { ctl?.cerrar(); await parar(p.proc); });

  const id = (nombre) => plugins.find((x) => x.nombre === nombre)?.id;

  test('hola: versión del protocolo, sistema y formatos', async () => {
    const h = await ctl.pedir('hola');
    assert.equal(h.ok, true);
    assert.equal(h.protocolo, 1);
    assert.match(h.puente, /^\d+\.\d+\.\d+$/);
    assert.ok(Array.isArray(h.capacidades) && h.capacidades.includes('capturarEditor') && h.capacidades.includes('escalarEditor'));
    assert.equal(typeof h.instancias, 'number');
    assert.equal(h.sistema, { linux: 'linux', win32: 'windows', darwin: 'mac' }[process.platform]);
    assert.ok(h.formatos.includes('VST3'));
    assert.ok(h.plugins >= 3);
  });

  test('listar: encuentra los tres plugins de prueba, con su tipo', () => {
    const s = plugins.find((x) => x.nombre === 'SynzPruebaSinte');
    const e = plugins.find((x) => x.nombre === 'SynzPruebaEfecto');
    assert.ok(s && e && plugins.find((x) => x.nombre === 'SynzPruebaRaro'));
    assert.equal(s.instrumento, true);
    assert.equal(e.instrumento, false);
    assert.equal(s.formato, 'VST3');
    assert.equal(s.fabricante, 'SYNZ');
  });

  test('crear sin plugin válido o con argumentos raros da error limpio', async () => {
    assert.equal((await ctl.pedir('crear', { plugin: 'no-existe' })).codigo, 'plugin_desconocido');
    assert.equal((await ctl.pedir('crear')).codigo, 'plugin_desconocido');
    assert.equal((await ctl.pedir('inventada')).codigo, 'operacion_desconocida');
    assert.equal((await ctl.pedir('destruir', { instancia: 999 })).codigo, 'instancia_desconocida');
    ctl.ws.send('esto no es json');
    ctl.ws.send('[1,2]');
    await esperar(150);
    assert.equal((await ctl.pedir('hola')).ok, true);     // y sigue vivo
  });

  test('instrumento: la nota empieza EXACTAMENTE en la muestra del MIDI y con la altura correcta', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaSinte'), frecuencia: 48000, bloqueMax: 512 });
    assert.equal(r.ok, true, JSON.stringify(r));
    assert.equal(r.instrumento, true);
    assert.equal(r.entradas, 0);
    assert.equal(r.salidas, 2);
    assert.equal(r.latencia, 0);
    assert.equal(r.editor, true);
    // Solo los parámetros automatizables: los más de 2000 «MIDI CC» que añade el envoltorio VST3 de JUCE no salen.
    assert.deepEqual(r.parametros.map((x) => x.nombre), ['Ganancia', 'Tipo', 'Bypass']);
    assert.equal(new Set(r.parametros.map((x) => x.id)).size, 3, 'ids distintos');
    assert.ok(r.parametros.every((x, k) => x.i === k && x.id !== '' && x.automatizable));
    assert.equal(r.parametros[1].pasos, 2);
    assert.ok(Math.abs(r.parametros[0].valor - 0.5) < 1e-6);

    const au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const b1 = await au.bloque({ muestras: 256, midi: [{ desp: 100, estado: 0x90, d1: 69, d2: 127 }] });
    assert.equal(b1.sec, 1);
    assert.equal(b1.canales, 2);
    assert.equal(b1.muestras, 256);
    for (let i = 0; i < 100; i++) assert.equal(b1.audio[0][i], 0, 'muestra ' + i + ' debería ser silencio');
    for (let i = 100; i < 256; i++) {
      assert.ok(Math.abs(b1.audio[0][i] - senoEsperado(i - 100)) < 2e-3, `muestra ${i}: ${b1.audio[0][i]} vs ${senoEsperado(i - 100)}`);
      assert.equal(b1.audio[0][i], b1.audio[1][i]);
    }
    // El siguiente bloque sigue la misma onda y la nota se suelta en la muestra 50.
    const b2 = await au.bloque({ muestras: 256, midi: [{ desp: 50, estado: 0x80, d1: 69, d2: 0 }] });
    for (let i = 0; i < 50; i++) assert.ok(Math.abs(b2.audio[0][i] - senoEsperado(156 + i)) < 2e-3, `bloque 2, muestra ${i}`);
    for (let i = 50; i < 256; i++) assert.equal(b2.audio[0][i], 0);
    assert.ok(b2.carga >= 0 && b2.carga < 10000);
    assert.equal(b2.banderas & 2, 0);
    au.cerrar();
    assert.equal((await ctl.pedir('destruir', { instancia: r.instancia })).ok, true);
  });

  test('instrumento: parámetros (ganancia y forma de onda) y varias notas en el mismo bloque, ordenadas por muestra', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaSinte'), frecuencia: 48000, bloqueMax: 512 });
    const au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    assert.equal((await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 0, valor: 1.0 })).ok, true);
    assert.deepEqual((await ctl.pedir('leerParametros', { instancia: r.instancia })).valores.map((x) => Math.round(x * 100) / 100), [1, 0, 0]);
    // Dos eventos desordenados: el puente los ordena. Nota 60 en la muestra 10 y su off en la 20; nota 69 en la 30.
    const b = await au.bloque({ muestras: 64, midi: [
      { desp: 30, estado: 0x90, d1: 69, d2: 127 },
      { desp: 20, estado: 0x80, d1: 60, d2: 0 },
      { desp: 10, estado: 0x90, d1: 60, d2: 127 },
    ] });
    for (let i = 0; i < 10; i++) assert.equal(b.audio[0][i], 0);
    for (let i = 10; i < 20; i++) assert.ok(Math.abs(b.audio[0][i]) > 0.0 || i === 10, 'suena la nota 60');
    assert.ok(Math.abs(b.audio[0][10] - 1.0) < 2e-3, 'ganancia a tope: la primera muestra vale 1');
    for (let i = 20; i < 30; i++) assert.equal(b.audio[0][i], 0, 'entre las dos notas hay silencio');
    assert.ok(Math.abs(b.audio[0][30] - 1.0) < 2e-3, 'la 69 entra en la muestra 30');
    // Cuadrada: valores ±1 exactos.
    await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 1, valor: 1.0 });
    await au.bloque({ muestras: 64, midi: [{ desp: 0, estado: 0x80, d1: 69, d2: 0 }] });
    const c = await au.bloque({ muestras: 64, midi: [{ desp: 0, estado: 0x90, d1: 69, d2: 127 }] });
    assert.ok(c.audio[0].every((x) => Math.abs(Math.abs(x) - 1.0) < 1e-6), 'la cuadrada vale +1 o -1');
    // Mensajes que no son de canal se ignoran; un desplazamiento fuera del bloque se acota.
    const d = await au.bloque({ muestras: 64, midi: [{ desp: 5, estado: 0xF8, long: 1 }, { desp: 9999, estado: 0x80, d1: 69 }] });
    assert.equal(d.muestras, 64);
    au.cerrar();
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('efecto: retardo real de 64 muestras, que el plugin declara; ganancia; entrada mono; saltar plugin', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 256 });
    assert.equal(r.ok, true, JSON.stringify(r));
    assert.equal(r.instrumento, false);
    assert.equal(r.entradas, 2);
    assert.equal(r.salidas, 2);
    assert.equal(r.latencia, 64);
    const au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const impulso = new Float32Array(128); impulso[0] = 1;
    const cero = new Float32Array(128);

    const b = await au.bloque({ muestras: 128, canales: [impulso, cero] });
    assert.equal(b.latencia, 64);
    for (let i = 0; i < 128; i++) assert.ok(Math.abs(b.audio[0][i] - (i === 64 ? 1 : 0)) < 1e-6, `canal 0, muestra ${i}`);
    assert.ok(b.audio[1].every((x) => x === 0));
    const b2 = await au.bloque({ muestras: 128, canales: [cero, cero] });
    assert.ok(b2.audio[0].every((x) => x === 0), 'el retardo no deja restos');

    // Un solo canal de entrada alimenta a los dos del plugin.
    const m = await au.bloque({ muestras: 128, canales: [impulso] });
    assert.ok(Math.abs(m.audio[0][64] - 1) < 1e-6 && Math.abs(m.audio[1][64] - 1) < 1e-6);
    await au.bloque({ muestras: 128, canales: [cero] });

    // Ganancia a tope = ×2.
    await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 0, valor: 1.0 });
    const g = await au.bloque({ muestras: 128, canales: [impulso, cero] });
    assert.ok(Math.abs(g.audio[0][64] - 2) < 1e-6);
    await au.bloque({ muestras: 128, canales: [cero, cero] });

    // Bandera de saltar el plugin: sale lo que entra, sin retardo.
    const s = await au.bloque({ muestras: 128, canales: [impulso, cero], banderas: 1 });
    assert.equal(s.audio[0][0], 1);
    assert.equal(s.audio[0][64], 0);
    au.cerrar();
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('estado: guardar, cambiar y restaurar; los cambios del plugin llegan como evento', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 44100, bloqueMax: 128 });
    const e0 = (await ctl.pedir('leerEstado', { instancia: r.instancia })).estado;
    assert.ok(typeof e0 === 'string' && e0.length > 10);
    await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 0, valor: 0.9 });
    assert.ok(Math.abs((await ctl.pedir('leerParametros', { instancia: r.instancia })).valores[0] - 0.9) < 1e-3);
    ctl.eventos.length = 0;
    const rest = await ctl.pedir('ponerEstado', { instancia: r.instancia, estado: e0 });
    assert.equal(rest.ok, true);
    assert.ok(Math.abs(rest.parametros[0].valor - 0.5) < 1e-3, 'el estado devuelve la ganancia a 0,5');
    const ev = await ctl.evento((m) => m.evento === 'parametro' && m.instancia === r.instancia && m.i === 0, 3000);
    assert.ok(Math.abs(ev.valor - 0.5) < 1e-3);
    // Lo que pone el estudio no se le devuelve como evento.
    ctl.eventos.length = 0;
    await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 0, valor: 0.25 });
    await esperar(300);
    assert.equal(ctl.eventos.filter((m) => m.evento === 'parametro').length, 0);
    assert.equal((await ctl.pedir('ponerEstado', { instancia: r.instancia, estado: '%%%' })).codigo, 'peticion_invalida');
    assert.equal((await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 99, valor: 1 })).codigo, 'peticion_invalida');
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('editor: la ventana nativa se abre y se cierra', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaSinte'), frecuencia: 48000, bloqueMax: 256 });
    assert.equal((await ctl.pedir('abrirEditor', { instancia: r.instancia })).ok, true);
    assert.equal((await ctl.pedir('abrirEditor', { instancia: r.instancia })).ok, true);   // otra vez: solo la trae al frente
    await esperar(300);
    assert.equal((await ctl.pedir('cerrarEditor', { instancia: r.instancia })).ok, true);
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('editor: se abre con zoom, se escala abierta y no se escala cerrada', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaSinte'), frecuencia: 48000, bloqueMax: 256 });
    const cerrada = await ctl.pedir('escalarEditor', { instancia: r.instancia, escala: 1.5 });
    assert.equal(cerrada.ok, false);
    assert.equal(cerrada.codigo, 'no_disponible');
    assert.equal((await ctl.pedir('abrirEditor', { instancia: r.instancia, escala: 1.25 })).ok, true);
    await esperar(300);
    assert.equal((await ctl.pedir('escalarEditor', { instancia: r.instancia, escala: 1.5 })).ok, true);
    await esperar(200);
    assert.equal((await ctl.pedir('cerrarEditor', { instancia: r.instancia })).ok, true);
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('«hola» cuenta las instancias abiertas: crear suma, destruir resta', async () => {
    const antes = (await ctl.pedir('hola')).instancias;
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 128 });
    assert.equal((await ctl.pedir('hola')).instancias, antes + 1);
    await ctl.pedir('destruir', { instancia: r.instancia });
    assert.equal((await ctl.pedir('hola')).instancias, antes);
  });

  test('foto de la ventana del plugin: sin abrir da error; abierta, un JPEG de verdad', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaSinte'), frecuencia: 48000, bloqueMax: 256 });
    const sin = await ctl.pedir('capturarEditor', { instancia: r.instancia });
    assert.equal(sin.ok, false);
    assert.equal(sin.codigo, 'no_disponible');
    await ctl.pedir('abrirEditor', { instancia: r.instancia });
    await esperar(500);
    const f = await ctl.pedir('capturarEditor', { instancia: r.instancia });
    if (process.platform === 'darwin') {      // en macOS la foto de la ventana aún no existe: el estudio pide entonces una imagen aparte
      assert.equal(f.ok, false);
      assert.equal(f.codigo, 'no_disponible');
      await ctl.pedir('cerrarEditor', { instancia: r.instancia });
      await ctl.pedir('destruir', { instancia: r.instancia });
      return;
    }
    assert.equal(f.ok, true);
    assert.equal(f.tipo, 'image/jpeg');
    assert.ok(f.ancho >= 16 && f.ancho <= 480 && f.alto >= 16, `tamaño ${f.ancho}x${f.alto}`);
    const bytes = Buffer.from(f.imagen, 'base64');
    assert.equal(bytes[0], 0xFF);
    assert.equal(bytes[1], 0xD8);              // cabecera JPEG
    assert.ok(bytes.length > 500, 'una imagen con contenido: ' + bytes.length);
    await ctl.pedir('cerrarEditor', { instancia: r.instancia });
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('un plugin que falla al procesar se silencia, avisa y no tumba al puente', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 128 });
    const au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const uno = new Float32Array(128).fill(0.5);
    const ok = await au.bloque({ muestras: 128, canales: [uno, uno] });
    assert.equal(ok.banderas & 2, 0);
    await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 1, valor: 1.0 });      // «romper»
    const roto = await au.bloque({ muestras: 128, canales: [uno, uno] });
    assert.equal(roto.banderas & 2, 2);
    assert.ok(roto.audio[0].every((x) => x === 0));
    const ev = await ctl.evento((m) => m.evento === 'instanciaCaida' && m.instancia === r.instancia, 3000);
    assert.match(ev.motivo, /excepción/);
    const otra = await au.bloque({ muestras: 128, canales: [uno, uno] });
    assert.equal(otra.banderas & 2, 2);
    assert.equal((await ctl.pedir('hola')).ok, true);
    au.cerrar();
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('seguridad: ticket, origen y dueño de la instancia', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 128 });
    // Ticket equivocado
    let au = await abrirAudio(p.puerto, r.instancia, 'ticket-falso', A);
    assert.equal(await au.cerrado, 1008);
    // Origen distinto (también permitido, pero no es el que la creó)
    au = await abrirAudio(p.puerto, r.instancia, r.ticket, B);
    assert.equal(await au.cerrado, 1008);
    // Instancia que no existe
    au = await abrirAudio(p.puerto, 12345, r.ticket, A);
    assert.equal(await au.cerrado, 1008);
    // Otra conexión de control no ve la instancia ajena
    const otro = await Control.abrir(p.puerto, A);
    assert.equal((await otro.pedir('destruir', { instancia: r.instancia })).codigo, 'instancia_desconocida');
    assert.equal((await otro.pedir('leerEstado', { instancia: r.instancia })).codigo, 'instancia_desconocida');
    otro.cerrar();
    // El buen canal sí, y solo uno
    const bueno = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const segundo = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    assert.equal(await segundo.cerrado, 1008);
    assert.equal((await bueno.bloque({ muestras: 64, canales: [new Float32Array(64)] })).muestras, 64);
    bueno.cerrar();
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('audio mal formado: se cierra con el código que toca y el puente sigue', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 128 });
    let au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    au.enviarCrudo(Buffer.alloc(10));
    assert.equal(await au.cerrado, 1002);
    await esperar(100);
    au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    au.ws.send('texto en el canal de audio');
    assert.equal(await au.cerrado, 1003);
    await esperar(100);
    au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const cab = Buffer.alloc(16);
    cab.writeUInt16LE(2, 4); cab.writeUInt16LE(5000, 6);            // bloque más grande que el máximo
    au.enviarCrudo(cab);
    assert.equal(await au.cerrado, 1002);
    await esperar(100);
    au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const cab2 = Buffer.alloc(16);
    cab2.writeUInt16LE(2, 4); cab2.writeUInt16LE(64, 6);            // dice 2 canales y no manda audio
    au.enviarCrudo(cab2);
    assert.equal(await au.cerrado, 1002);
    assert.equal((await ctl.pedir('hola')).ok, true);
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('cuatro instancias a la vez, cada una con su hilo, sin mezclarse', async () => {
    const ins = [];
    for (let k = 0; k < 4; k++) {
      const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 128 });
      await ctl.pedir('ponerParametro', { instancia: r.instancia, i: 0, valor: (k + 1) / 8 });   // ganancia = (k+1)/4
      ins.push({ r, au: await abrirAudio(p.puerto, r.instancia, r.ticket, A) });
    }
    const impulso = new Float32Array(128); impulso[0] = 1;
    const cero = new Float32Array(128);
    await Promise.all(ins.map(async ({ au }, k) => {
      for (let n = 0; n < 60; n++) {
        const b = await au.bloque({ muestras: 128, canales: [n % 2 === 0 ? impulso : cero, cero] });
        const esperado = n % 2 === 0 ? ((k + 1) / 4) : 0;     // el impulso entra en los bloques pares y sale 64 muestras después, en el mismo bloque
        assert.ok(Math.abs(b.audio[0][64] - esperado) < 1e-6, `instancia ${k}, bloque ${n}: ${b.audio[0][64]} vs ${esperado}`);
      }
    }));
    ins.forEach(({ au }) => au.cerrar());
    for (const { r } of ins) await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('tiempo de ida y vuelta de un bloque (informativo)', async () => {
    const r = await ctl.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 256 });
    const au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    const x = new Float32Array(128);
    for (let i = 0; i < 50; i++) await au.bloque({ muestras: 128, canales: [x, x] });   // calentar
    const t = [];
    for (let i = 0; i < 600; i++) {
      const t0 = process.hrtime.bigint();
      await au.bloque({ muestras: 128, canales: [x, x] });
      t.push(Number(process.hrtime.bigint() - t0) / 1e6);
    }
    t.sort((a, b) => a - b);
    const media = t.reduce((a, b) => a + b, 0) / t.length;
    console.log(`  ida y vuelta de 128 muestras: media ${media.toFixed(3)} ms, p50 ${t[300].toFixed(3)}, p99 ${t[594].toFixed(3)}, máx ${t[599].toFixed(3)} (un bloque dura 2,667 ms a 48 kHz)`);
    assert.ok(media < 5, 'la media debería ser mucho menor que lo que dura el bloque');
    au.cerrar();
    await ctl.pedir('destruir', { instancia: r.instancia });
  });

  test('al cerrarse la conexión de control se descargan sus instancias y se corta su audio', async () => {
    const otro = await Control.abrir(p.puerto, A);
    const r = await otro.pedir('crear', { plugin: id('SynzPruebaEfecto'), frecuencia: 48000, bloqueMax: 128 });
    const au = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    await au.bloque({ muestras: 64, canales: [new Float32Array(64)] });
    otro.cerrar();
    await Promise.race([au.cerrado, esperar(4000).then(() => { throw new Error('el audio no se cortó'); })]);
    const nuevo = await abrirAudio(p.puerto, r.instancia, r.ticket, A);
    assert.equal(await nuevo.cerrado, 1008, 'la instancia ya no existe');
  });
});

describe('límites', () => {
  test('límite de instancias: la número 33 se rechaza, y al soltar vuelve a haber hueco', async () => {
    const p = await arrancarPuente();
    const c = await Control.abrir(p.puerto, A);
    try {
      await esperarEscaneo(c, 3);
      const plugin = (await c.pedir('listar')).plugins.find((x) => x.nombre === 'SynzPruebaEfecto').id;
      const ids = [];
      for (let i = 0; i < 32; i++) {
        const r = await c.pedir('crear', { plugin, frecuencia: 48000, bloqueMax: 64 });
        assert.equal(r.ok, true, 'instancia ' + i + ': ' + JSON.stringify(r));
        ids.push(r.instancia);
      }
      assert.equal((await c.pedir('crear', { plugin, frecuencia: 48000, bloqueMax: 64 })).codigo, 'limite');
      await c.pedir('destruir', { instancia: ids[0] });
      assert.equal((await c.pedir('crear', { plugin, frecuencia: 48000, bloqueMax: 64 })).ok, true);
    } finally { c.cerrar(); await parar(p.proc); }
  });

  test('con el puerto ocupado, el segundo puente lo dice y sale con error', async () => {
    const p = await arrancarPuente();
    try {
      const { spawn } = await import('node:child_process');
      const hijo = spawn(...sinXvfb('xvfb-run', ['-a', EXE, '--sin-ventana', '--puerto', String(p.puerto), '--datos', fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-'))]), { detached: process.platform === 'linux' });
      let errores = '';
      hijo.stderr.on('data', (d) => { errores += d.toString(); });
      hijo.stdout.on('data', (d) => { errores += d.toString(); });   // xvfb-run junta las dos salidas
      const codigo = await new Promise((res) => {
        hijo.on('close', res);
        setTimeout(() => { try { process.kill(-hijo.pid, 'SIGKILL'); } catch {} res('colgado'); }, 20000).unref();
      });
      assert.equal(codigo, 2);
      assert.match(errores, /no se pudo abrir el puerto/);
    } finally { await parar(p.proc); }
  });
});

describe('abrir al iniciar sesión', () => {
  test('--inicio-automatico si|no pone y quita la entrada de arranque, en segundo plano', { skip: process.platform !== 'linux' && 'la entrada de arranque se mira en Linux (en Windows es el registro)' }, async () => {
    const casa = fs.mkdtempSync(path.join(os.tmpdir(), 'puente-casa-'));
    const corre = (valor) => new Promise((res) => {
      const h = spawn('xvfb-run', ['-a', EXE, '--inicio-automatico', valor], { env: { ...process.env, HOME: casa } });
      let sale = ''; h.stdout.on('data', (d) => { sale += d; }); h.on('close', (c) => res({ c, sale }));
    });
    const f = path.join(casa, '.config', 'autostart', 'synz-puente.desktop');
    const si = await corre('si');
    assert.equal(si.c, 0); assert.match(si.sale, /INICIO-AUTOMATICO si/);
    const t = fs.readFileSync(f, 'utf8');
    assert.match(t, /Exec=.*--en-segundo-plano/);
    assert.match(t, /Name=SYNZ Bridge/);
    const no = await corre('no');
    assert.equal(no.c, 0); assert.match(no.sale, /INICIO-AUTOMATICO no/);
    assert.equal(fs.existsSync(f), false);
  });
});

describe('abrir desde el estudio (enlace synz-bridge://)', () => {
  test('--registrar-enlace deja el esquema registrado (Linux: .desktop con %u)', { skip: process.platform !== 'linux' && 'en Windows es el registro' }, async () => {
    const casa = fs.mkdtempSync(path.join(os.tmpdir(), 'puente-casa-'));
    const h = spawn('xvfb-run', ['-a', EXE, '--registrar-enlace'], { env: { ...process.env, HOME: casa } });
    let sale = ''; h.stdout.on('data', (d) => { sale += d; });
    const c = await new Promise((r) => h.on('close', r));
    assert.equal(c, 0); assert.match(sale, /ENLACE registrado/);
    const t = fs.readFileSync(path.join(casa, '.local', 'share', 'applications', 'synz-bridge.desktop'), 'utf8');
    assert.match(t, /Exec=".*" %u/);
    assert.match(t, /x-scheme-handler\/synz-bridge/);
  });

  test('si el estudio lo abre y ya había uno abierto, el segundo se va sin avisar de nada', { skip: process.platform !== 'linux' && 'se prueba en Linux' }, async () => {
    const p = await arrancarPuente();
    try {
      const h = spawn('xvfb-run', ['-a', EXE, 'synz-bridge://abrir', '--puerto', String(p.puerto), '--datos', fs.mkdtempSync(path.join(os.tmpdir(), 'puente-datos-'))]);
      const c = await new Promise((res) => { h.on('close', res); setTimeout(() => { try { h.kill('SIGKILL'); } catch {} res('colgado'); }, 15000).unref(); });
      assert.equal(c, 0, 'sin diálogo de «ya hay otro»: sale solo');
      assert.equal((await (await Control.abrir(p.puerto, A)).pedir('hola')).ok, true, 'el primero sigue vivo');
    } finally { await parar(p.proc); }
  });
});

describe('plugin con muchas salidas (como Kontakt)', () => {
  test('se carga con la salida principal y deja las demás apagadas', async () => {
    const p = await arrancarPuente({ env: { SYNZ_PRUEBA_SALIDAS: '8' } });      // 9 salidas estéreo = 18 canales, más de las 8 que admite el puente
    const c = await Control.abrir(p.puerto, A);
    try {
      await esperarEscaneo(c, 3);
      const sinte = (await c.pedir('listar')).plugins.find((x) => x.nombre === 'SynzPruebaSinte');
      const r = await c.pedir('crear', { plugin: sinte.id, frecuencia: 48000, bloqueMax: 256 });
      assert.equal(r.ok, true, JSON.stringify(r));
      assert.equal(r.salidas, 2);
    } finally { c.cerrar(); await parar(p.proc); }
  });
});

describe('escáner a prueba de plugins rotos', () => {
  test('un plugin que se cae al cargarse no tumba el escaneo ni el puente; queda en lista negra', async () => {
    const p = await arrancarPuente({ env: { SYNZ_PRUEBA_RARO: 'abortar' } });
    const c = await Control.abrir(p.puerto, A);
    try {
      await esperarEscaneo(c, 2);
      c.eventos.length = 0;
      await c.pedir('escanear', { forzar: true });
      const fin = await c.evento((m) => m.evento === 'escaneoTerminado', 60000);
      assert.ok(fin.total >= 2);
      const malo = fin.fallidos.find((f) => f.archivo === 'SynzPruebaRaro.vst3');
      assert.ok(malo, JSON.stringify(fin));
      assert.match(malo.motivo, /cayó|código/);
      const nombres = (await c.pedir('listar')).plugins.filter((x) => x.formato === 'VST3').map((x) => x.nombre).sort();
      assert.deepEqual(nombres, ['SynzPruebaEfecto', 'SynzPruebaSinte']);

      // Sin forzar, el que falló se salta sin volver a lanzar nada.
      c.eventos.length = 0;
      const t0 = Date.now();
      await c.pedir('escanear', {});
      const fin2 = await c.evento((m) => m.evento === 'escaneoTerminado', 20000);
      assert.match(fin2.fallidos.find((f) => f.archivo === 'SynzPruebaRaro.vst3').motivo, /se salta/);
      console.log(`  reescaneo sin forzar con un plugin en lista negra: ${Date.now() - t0} ms`);
    } finally { c.cerrar(); await parar(p.proc); }

    // Otra vez con el mismo dato y sin el fallo: hasta que no se fuerza, el plugin sigue apartado; al forzar, aparece.
    const q = await arrancarPuente({ datos: p.datos });
    const d = await Control.abrir(q.puerto, A);
    try {
      const antes = (await d.pedir('listar')).plugins.map((x) => x.nombre);
      assert.ok(antes.includes('SynzPruebaSinte'), 'la caché ya trae los plugins sin esperar al escaneo');
      await esperarEscaneo(d, 2);
      assert.ok(!(await d.pedir('listar')).plugins.some((x) => x.nombre === 'SynzPruebaRaro'));
      d.eventos.length = 0;
      await d.pedir('escanear', { forzar: true });
      await d.evento((m) => m.evento === 'escaneoTerminado', 60000);
      assert.ok((await d.pedir('listar')).plugins.some((x) => x.nombre === 'SynzPruebaRaro'));
    } finally { d.cerrar(); await parar(q.proc); }
  });

  test('un plugin que se cuelga al cargarse se corta al pasar el plazo', async () => {
    const p = await arrancarPuente({ env: { SYNZ_PRUEBA_RARO: 'colgar' }, extra: ['--plazo-escaneo-ms', '3000'] });
    const c = await Control.abrir(p.puerto, A);
    try {
      const t0 = Date.now();
      await esperarEscaneo(c, 2);
      c.eventos.length = 0;
      await c.pedir('escanear', { forzar: true });
      const fin = await c.evento((m) => m.evento === 'escaneoTerminado', 60000);
      const malo = fin.fallidos.find((f) => f.archivo === 'SynzPruebaRaro.vst3');
      assert.match(malo.motivo, /tardó más de 3 s/);
      assert.ok(fin.total >= 2);
      assert.ok(Date.now() - t0 < 40000);
      assert.equal((await c.pedir('hola')).ok, true);
    } finally { c.cerrar(); await parar(p.proc); }
  });
});
