// Cliente del puente de plugins nativos para el navegador. Protocolo: PROTOCOLO.md.
//
//   const puente = await PuenteCliente.conectar();            // ws://127.0.0.1:47821
//   const lista  = await puente.plugins();
//   const inst   = await puente.crear(ctx, lista[0].id);      // inst.out se conecta a la mesa como cualquier nodo
//   inst.noteOn(60, 0.8, ctx.currentTime + 0.1);              // MIDI con el tiempo del AudioContext
//   await inst.abrirEditor();                                 // la ventana nativa del plugin
//   await inst.dispose();
//
// No necesita SharedArrayBuffer ni cabeceras especiales: el worklet habla con su Worker por un MessagePort.
export const URL_POR_DEFECTO = 'ws://127.0.0.1:47821';
export const PROTOCOLO = 1;

export class ErrorPuente extends Error {
  constructor(codigo, mensaje) {
    super(mensaje);
    this.name = 'ErrorPuente';
    this.codigo = codigo;
  }
}

const worklets = new WeakMap();      // AudioContext → promesa de addModule

function cargarWorklet(ctx, url) {
  if (!worklets.has(ctx)) worklets.set(ctx, ctx.audioWorklet.addModule(url));
  return worklets.get(ctx);
}

export class PuenteCliente {
  /** Se conecta al puente y comprueba que habla el mismo protocolo. Rechaza con `ErrorPuente('no_disponible')` si no hay puente. */
  static conectar({ url = URL_POR_DEFECTO, plazoMs = 4000 } = {}) {
    return new Promise((resolve, reject) => {
      let ws;
      try {
        ws = new WebSocket(url + '/control');
      } catch (e) {
        reject(new ErrorPuente('no_disponible', 'dirección del puente no válida'));
        return;
      }
      const cliente = new PuenteCliente(ws, url);
      const sinPuente = () => { clearTimeout(temporizador); reject(new ErrorPuente('no_disponible', 'no se pudo conectar con el puente (¿está abierto?)')); };
      const temporizador = setTimeout(() => { ws.close(); reject(new ErrorPuente('no_disponible', 'el puente no contesta')); }, plazoMs);
      ws.addEventListener('close', sinPuente);      // solo cuenta hasta que se conecta; después el cierre lo lleva `#cerrada`
      ws.addEventListener('open', async () => {
        try {
          const hola = await cliente.pedir('hola', { protocolo: PROTOCOLO });
          if (hola.protocolo !== PROTOCOLO) throw new ErrorPuente('version', `el puente habla el protocolo ${hola.protocolo} y este cliente el ${PROTOCOLO}`);
          clearTimeout(temporizador);
          ws.removeEventListener('close', sinPuente);
          cliente.hola = hola;
          resolve(cliente);
        } catch (e) {
          clearTimeout(temporizador);
          ws.removeEventListener('close', sinPuente);
          ws.close();
          reject(e);
        }
      });
    });
  }

  constructor(ws, url) {
    this.ws = ws;
    this.url = url;
    this.hola = null;
    this.n = 1;
    this.pendientes = new Map();
    this.oyentes = new Set();
    this.alDesconectar = null;
    this._cierres = new Set();
    this.instancias = new Map();
    this.conectado = true;
    ws.onmessage = (e) => this.#mensaje(e);
    ws.onclose = () => this.#cerrada();
  }

  #mensaje(e) {
    if (typeof e.data !== 'string') return;
    let m;
    try { m = JSON.parse(e.data); } catch { return; }
    if (m.evento) {
      for (const f of this.oyentes) try { f(m); } catch (err) { console.error(err); }
      const inst = this.instancias.get(m.instancia);
      if (inst) inst._evento(m);
      return;
    }
    const p = this.pendientes.get(m.id);
    if (!p) return;
    this.pendientes.delete(m.id);
    clearTimeout(p.t);
    if (m.ok) p.res(m); else p.rej(new ErrorPuente(m.codigo || 'error', m.error || 'error del puente'));
  }

  #cerrada() {
    this.conectado = false;
    for (const [, p] of this.pendientes) { clearTimeout(p.t); p.rej(new ErrorPuente('no_disponible', 'se cortó la conexión con el puente')); }
    this.pendientes.clear();
    for (const inst of [...this.instancias.values()]) inst._cayo('se cortó la conexión con el puente');
    this.instancias.clear();
    if (this.alDesconectar) this.alDesconectar();
    for (const f of [...this._cierres]) try { f(); } catch (err) { console.error(err); }
  }

  /** Avisa cuando se corta la conexión con el puente (varios oyentes: el instrumento y los efectos comparten una). Devuelve la función para dejar de oír. */
  alCerrarse(f) {
    this._cierres.add(f);
    return () => this._cierres.delete(f);
  }

  /** Una petición de control. Devuelve la respuesta (con `ok: true`) o rechaza con ErrorPuente. */
  pedir(op, args = {}, plazoMs = 30000) {
    return new Promise((res, rej) => {
      if (!this.conectado) return rej(new ErrorPuente('no_disponible', 'no hay conexión con el puente'));
      const id = this.n++;
      const t = setTimeout(() => { this.pendientes.delete(id); rej(new ErrorPuente('plazo', `el puente no contestó a «${op}»`)); }, plazoMs);
      this.pendientes.set(id, { res, rej, t });
      this.ws.send(JSON.stringify({ id, op, ...args }));
    });
  }

  /** Eventos sin petición (escaneo, parámetros, editor…). Devuelve la función para dejar de oír. */
  alEvento(f) {
    this.oyentes.add(f);
    return () => this.oyentes.delete(f);
  }

  async plugins() { return (await this.pedir('listar')).plugins; }
  async escanear(forzar = false) { return this.pedir('escanear', { forzar }); }

  /**
   * Carga un plugin y devuelve su instancia. `opciones`: `bloque` (muestras por petición, múltiplo de 128; 128 por defecto),
   * `colchon` (muestras guardadas antes de sonar, 2 bloques por defecto; si hay cortes sube solo, un bloque cada vez, hasta `colchonMax`, que es el cuádruple por defecto), `entradas`, `salidas`, y `workletUrl` /
   * `trabajadorUrl` si los archivos no están junto a este.
   */
  async crear(ctx, pluginId, opciones = {}) {
    const bloque = opciones.bloque ?? 128;
    const colchon = opciones.colchon ?? 2 * bloque;
    if (bloque % 128 !== 0 || bloque < 128 || bloque > 2048) throw new ErrorPuente('peticion_invalida', 'el bloque tiene que ser un múltiplo de 128 entre 128 y 2048');
    const info = await this.pedir('crear', { plugin: pluginId, frecuencia: ctx.sampleRate, bloqueMax: bloque, entradas: opciones.entradas, salidas: opciones.salidas });
    try {
      await cargarWorklet(ctx, opciones.workletUrl ?? new URL('./puente-worklet.js', import.meta.url));
      const inst = new InstanciaPuente(this, ctx, info, bloque, colchon, opciones);
      this.instancias.set(info.instancia, inst);
      await inst._abrirAudio();
      return inst;
    } catch (e) {
      this.instancias.delete(info.instancia);
      this.pedir('destruir', { instancia: info.instancia }).catch(() => {});
      throw e;
    }
  }

  cerrar() {
    this.ws.close();
  }
}

export class InstanciaPuente {
  constructor(cliente, ctx, info, bloque, colchon, opciones) {
    this.cliente = cliente;
    this.ctx = ctx;
    this.id = info.instancia;
    this.nombre = info.nombre;
    this.instrumento = info.instrumento;
    this.tieneEditor = info.editor;
    this.parametros = info.parametros;
    this.bloque = bloque;
    this.colchon = colchon;
    this.latenciaPlugin = info.latencia;
    this.estado = { cortes: 0, perdidos: 0, descartes: 0, enviados: 0, recibidos: 0, guardado: 0, carga: 0, banderas: 0, rttMs: 0, rttMaxMs: 0 };
    this.caida = false;
    this.disposed = false;
    this._oyentes = { parametro: new Set(), editorCerrado: new Set(), caida: new Set() };

    const entradas = Math.min(2, info.entradas);
    const salidas = Math.max(1, Math.min(2, info.salidas));
    // Nada de crear antes de usar: el nodo y el Worker nacen con la instancia y mueren con ella (dispose).
    this.nodo = new AudioWorkletNode(ctx, 'synz-puente', {
      numberOfInputs: entradas > 0 ? 1 : 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      channelCount: 2,
      channelCountMode: 'explicit',
      channelInterpretation: 'speakers',
      processorOptions: { bloque, entradas, salidas, colchon, latenciaPlugin: info.latencia, calibrar: opciones.calibrar !== false, colchonMax: opciones.colchonMax ?? 4 * Math.max(colchon, bloque) },
    });
    this.nodo.port.onmessage = (e) => {
      if (e.data.tipo === 'calibrada') {
        if (e.data.muestras >= 0) this.conducto = e.data.muestras;
        this._alCalibrar?.(this.conducto);
      } else if (e.data.tipo === 'colchon') {
        // El colchón creció solo: el retardo medido sube lo mismo.
        if (this.conducto != null) this.conducto += e.data.muestras - this.colchon;
        this.colchon = e.data.muestras;
      } else if (e.data.tipo === 'stats') {
        const { tipo, latenciaPlugin, ...resto } = e.data;
        Object.assign(this.estado, resto);
        this.latenciaPlugin = latenciaPlugin;
      }
    };
    this._trabajadorUrl = opciones.trabajadorUrl ?? new URL('./puente-trabajador.js', import.meta.url);
    this._urlAudio = `${cliente.url}/audio/${info.instancia}?t=${info.ticket}`;
  }

  /** Nodo al que se conecta la mesa (y, si es un efecto, también donde entra el audio). */
  get out() { return this.nodo; }
  get in() { return this.nodo; }

  _abrirAudio() {
    const canal = new MessageChannel();
    this.trabajador = new Worker(this._trabajadorUrl);
    return new Promise((res, rej) => {
      const t = setTimeout(() => rej(new ErrorPuente('plazo', 'el canal de audio del puente no se abrió')), 5000);
      this.trabajador.onmessage = (e) => {
        const m = e.data;
        if (m.tipo === 'abierto') {
          clearTimeout(t);
          // El nodo recibe su puerto cuando el canal ya está abierto: así ningún bloque (ni la calibración) se pierde antes.
          this.nodo.port.postMessage({ tipo: 'puerto', puerto: canal.port1 }, [canal.port1]);
          res();
        }
        else if (m.tipo === 'rtt') { this.estado.rttMs = m.media; this.estado.rttMaxMs = m.maximo; }
        else if (m.tipo === 'cerrado') {
          clearTimeout(t);
          if (!m.esperado) { if (!this.disposed) this._cayo(m.codigo === 1008 ? 'el puente rechazó el canal de audio' : 'se cortó el canal de audio'); rej(new ErrorPuente('no_disponible', `el canal de audio se cerró (${m.codigo})`)); }
        }
      };
      this.trabajador.postMessage({ tipo: 'iniciar', url: this._urlAudio, puerto: canal.port2 }, [canal.port2]);
    });
  }

  //==============================================================================
  // MIDI. `tiempo` es un instante de `ctx.currentTime`; sin él, lo antes posible.
  midi(bytes, tiempo) {
    if (this.disposed) return;
    const frame = tiempo == null ? 0 : Math.round(tiempo * this.ctx.sampleRate);
    this.nodo.port.postMessage({ tipo: 'midi', frame, bytes });
  }
  noteOn(nota, velocidad = 0.8, tiempo, canal = 0) {
    this.midi([0x90 | (canal & 15), nota & 127, Math.max(1, Math.min(127, Math.round(velocidad * 127)))], tiempo);
  }
  noteOff(nota, tiempo, canal = 0) {
    this.midi([0x80 | (canal & 15), nota & 127, 0], tiempo);
  }
  controlador(numero, valor0a1, tiempo, canal = 0) {
    this.midi([0xB0 | (canal & 15), numero & 127, Math.max(0, Math.min(127, Math.round(valor0a1 * 127)))], tiempo);
  }
  todasLasNotasOff() {
    this.midi([0xB0, 123, 0]);
    this.midi([0xB0, 120, 0]);
  }

  /** Saltar el plugin: sale lo que entra, sin pasar por él. */
  saltar(activo) {
    this.nodo.port.postMessage({ tipo: 'saltar', activo });
  }

  //==============================================================================
  /** Retardo total que añade el puente, en muestras: el del conducto (medido al conectar; hasta entonces, colchón + bloque) y el propio del plugin. */
  latenciaMuestras() {
    return (this.conducto ?? this.colchon + this.bloque) + this.latenciaPlugin;
  }
  /** Espera a que el nodo, ya conectado a la mesa, mida su retardo real (tarda unos milisegundos). Devuelve las muestras o null. */
  esperarCalibracion(plazoMs = 6000) {
    if (this.conducto != null) return Promise.resolve(this.conducto);
    return new Promise((res) => {
      const t = setTimeout(() => { this._alCalibrar = null; res(null); }, plazoMs);
      this._alCalibrar = (v) => { clearTimeout(t); this._alCalibrar = null; res(v ?? null); };
    });
  }
  latenciaMs() {
    return (this.latenciaMuestras() / this.ctx.sampleRate) * 1000;
  }

  //==============================================================================
  // Parámetros, estado y ventana
  ponerParametro(i, valor) { return this.cliente.pedir('ponerParametro', { instancia: this.id, i, valor }); }
  async leerParametros() { return (await this.cliente.pedir('leerParametros', { instancia: this.id })).valores; }
  async leerEstado() { return (await this.cliente.pedir('leerEstado', { instancia: this.id })).estado; }
  async ponerEstado(base64) {
    const r = await this.cliente.pedir('ponerEstado', { instancia: this.id, estado: base64 });
    this.parametros = r.parametros;
    return r.parametros;
  }
  /** `escala`: zoom de la ventana (1 = normal); lo respetan los plugins que admiten escala. */
  async abrirEditor(escala) { const r = await this.cliente.pedir('abrirEditor', { instancia: this.id, ...(escala ? { escala } : {}) }); this.editorAbierto = true; return r; }
  /** Cambia el zoom de la ventana ya abierta (0,25 a 3). */
  escalarEditor(escala) { return this.cliente.pedir('escalarEditor', { instancia: this.id, escala }); }
  async cerrarEditor() { this.editorAbierto = false; return this.cliente.pedir('cerrarEditor', { instancia: this.id }); }
  /** Foto de la ventana del plugin ya abierta: `{ imagen (JPEG en base64), tipo, ancho, alto }`. */
  capturarEditor() { return this.cliente.pedir('capturarEditor', { instancia: this.id }); }
  /** Abre la ventana si hace falta, espera a que el plugin se pinte, hace la foto y deja la ventana como estaba. */
  async fotografiar(esperaMs = 900) {
    const estabaAbierta = !!this.editorAbierto;
    if (!estabaAbierta) await this.abrirEditor();
    try {
      await new Promise((r) => setTimeout(r, esperaMs));
      return await this.capturarEditor();
    } finally {
      if (!estabaAbierta) await this.cerrarEditor().catch(() => {});
    }
  }

  alParametro(f) { this._oyentes.parametro.add(f); return () => this._oyentes.parametro.delete(f); }
  alCerrarseElEditor(f) { this._oyentes.editorCerrado.add(f); return () => this._oyentes.editorCerrado.delete(f); }
  /** El plugin falló o se perdió la conexión: la instancia ya no suena. */
  alCaer(f) { this._oyentes.caida.add(f); return () => this._oyentes.caida.delete(f); }

  _evento(m) {
    if (m.evento === 'parametro') this._oyentes.parametro.forEach((f) => f(m.i, m.valor));
    else if (m.evento === 'editorCerrado') { this.editorAbierto = false; this._oyentes.editorCerrado.forEach((f) => f()); }
    else if (m.evento === 'instanciaCaida') this._cayo(m.motivo);
  }

  _cayo(motivo) {
    if (this.caida) return;
    this.caida = true;
    this._oyentes.caida.forEach((f) => f(motivo));
  }

  /** Suelta todo: desconecta el nodo, cierra el audio y descarga el plugin en el puente. */
  async dispose() {
    if (this.disposed) return;
    this.disposed = true;
    try { this.nodo.disconnect(); } catch { /* ya estaba suelto */ }
    this.nodo.port.postMessage({ tipo: 'cerrar' });
    if (this.trabajador) {
      this.trabajador.postMessage({ tipo: 'cerrar' });
      setTimeout(() => this.trabajador.terminate(), 300);
    }
    this.cliente.instancias.delete(this.id);
    if (this.cliente.conectado) await this.cliente.pedir('destruir', { instancia: this.id }).catch(() => {});
  }
}
