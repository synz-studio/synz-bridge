// AudioWorklet del puente. Va dentro de un nodo de audio del estudio y hace de «cable» hacia el plugin nativo:
//   • junta la entrada en bloques de `bloque` muestras y les pega el MIDI que cae dentro de cada uno, con su
//     desplazamiento exacto en la muestra;
//   • manda cada bloque al Worker por un MessagePort directo (sin pasar por el hilo principal) y recibe la respuesta;
//   • reproduce la respuesta desde un anillo, empezando cuando hay `colchon` muestras guardadas.
// No usa SharedArrayBuffer, así que funciona sin cabeceras COOP/COEP. Formato de los mensajes: PROTOCOLO.md.
const CUANTO = 128;          // el navegador procesa de 128 en 128 muestras
const MAX_MIDI = 64;         // eventos MIDI por bloque; lo que sobre pasa al bloque siguiente

class ProcesadorPuente extends AudioWorkletProcessor {
  constructor(opciones) {
    super();
    const o = opciones.processorOptions;
    this.bloque = o.bloque;                       // múltiplo de 128
    this.entradas = o.entradas;                   // canales de entrada que se mandan (0, 1 o 2)
    this.salidas = o.salidas;                     // canales que devuelve el plugin (1 o 2 se usan)
    this.colchon = o.colchon;
    this.colchonMax = o.colchonMax | 0;           // tope al que sube solo el colchón si hay cortes (0 = fijo)
    this.vivo = true;
    this.saltar = false;
    // Calibración: con el conducto ya asentado manda un bloque con un impulso y el plugin saltado, y mide cuántas muestras
    // pasan hasta que ese impulso sale por la salida. Da el retardo real del conducto (dispositivo, colchón, bloque),
    // que no se sabe de antemano. Estados: pendiente → enviando → esperando → enSalida → hecha.
    this.calib = o.calibrar === false ? 'hecha' : 'pendiente';
    this.impulsoFrame = 0;
    this.secCalib = 0;
    this.posImpulso = 0;
    this.intentos = 0;
    this.frameCanal = 0;                          // `currentFrame` en que llegó el canal de audio

    this.acum = [];
    for (let c = 0; c < this.entradas; c++) this.acum.push(new Float32Array(this.bloque));
    this.n = 0;                                   // muestras ya acumuladas del bloque en curso
    this.inicio = 0;                              // `currentFrame` donde empezó el bloque en curso

    let cap = 4096;
    while (cap < 8 * (this.bloque + Math.max(this.colchon, this.colchonMax))) cap *= 2;
    this.cap = cap;
    this.mascara = cap - 1;
    this.anillo = [new Float32Array(cap), new Float32Array(cap)];
    this.esc = 0;
    this.lec = 0;
    this.cebado = false;

    this.midi = [];                               // { frame, e0, e1, e2, largo }, ordenado por frame
    this.secuencia = 1;
    this.puerto = null;

    this.peticion = new ArrayBuffer(16 + MAX_MIDI * 8 + 2 * this.bloque * 4);
    this.dv = new DataView(this.peticion);
    this.u8 = new Uint8Array(this.peticion);

    // estadísticas
    this.cortes = 0;                              // veces que se quedó sin audio y calló
    this.perdidos = 0;                            // bloques que no se pudieron mandar
    this.descartes = 0;                           // veces que se tiró audio para no acumular retardo
    this.bloquesEnviados = 0;
    this.bloquesRecibidos = 0;
    this.latenciaPlugin = o.latenciaPlugin | 0;
    this.carga = 0;
    this.banderas = 0;
    this.contador = 0;

    this.port.onmessage = (e) => this.mensajeDelHiloPrincipal(e.data);
  }

  mensajeDelHiloPrincipal(m) {
    switch (m.tipo) {
      case 'puerto':
        this.puerto = m.puerto;
        this.frameCanal = currentFrame;
        this.perdidos = 0;                        // lo contado antes de tener canal es el arranque, no una pérdida
        this.puerto.onmessage = (e) => this.mensajeDelTrabajador(e.data);
        break;
      case 'midi': {
        const ev = { frame: m.frame, e0: m.bytes[0] & 255, e1: (m.bytes[1] ?? 0) & 255, e2: (m.bytes[2] ?? 0) & 255, largo: Math.min(3, m.bytes.length) };
        let i = this.midi.length;
        while (i > 0 && this.midi[i - 1].frame > ev.frame) i--;
        this.midi.splice(i, 0, ev);
        break;
      }
      case 'saltar': this.saltar = !!m.activo; break;
      case 'colchon': this.colchon = m.muestras | 0; break;
      case 'cerrar': this.vivo = false; break;
    }
  }

  mensajeDelTrabajador(m) {
    if (m.tipo !== 'respuesta') return;
    const b = m.datos;
    if (!(b instanceof ArrayBuffer) || b.byteLength < 16) return;
    const dv = new DataView(b);
    const canales = dv.getUint16(4, true);
    const muestras = dv.getUint16(6, true);
    if (canales < 1 || b.byteLength !== 16 + canales * muestras * 4) return;
    this.latenciaPlugin = dv.getUint32(8, true);
    this.carga = dv.getUint16(12, true);
    this.banderas = dv.getUint16(14, true);
    this.bloquesRecibidos++;
    if (this.calib === 'esperando' && dv.getUint32(0, true) === this.secCalib) { this.posImpulso = this.esc; this.calib = 'enSalida'; }

    const datos = new Float32Array(b, 16, canales * muestras);
    if (this.cap - (this.esc - this.lec) < muestras) { this.descartes++; this.lec = this.esc - this.colchon; }   // anillo lleno: se tira lo viejo
    for (let k = 0; k < 2; k++) {
      const src = datos.subarray(Math.min(k, canales - 1) * muestras, Math.min(k, canales - 1) * muestras + muestras);   // un canal solo suena por los dos lados
      const dst = this.anillo[k];
      for (let i = 0; i < muestras; i++) dst[(this.esc + i) & this.mascara] = src[i];
    }
    this.esc += muestras;
  }

  enviar() {
    if (this.puerto === null) { this.perdidos++; this.midi.length = 0; return; }
    const fin = this.inicio + this.bloque;
    const calibrar = this.calib === 'enviando';
    let nMidi = 0;
    let o = 16;
    while (!calibrar && this.midi.length > 0 && this.midi[0].frame < fin && nMidi < MAX_MIDI) {
      const e = this.midi.shift();
      this.dv.setUint16(o, Math.max(0, Math.min(this.bloque - 1, e.frame - this.inicio)), true);
      this.u8[o + 2] = e.e0; this.u8[o + 3] = e.e1; this.u8[o + 4] = e.e2; this.u8[o + 5] = e.largo;
      this.dv.setUint16(o + 6, 0, true);
      o += 8;
      nMidi++;
    }
    const canales = calibrar ? 1 : this.entradas;
    for (let c = 0; c < canales; c++) {
      const dst = new Float32Array(this.peticion, o + c * this.bloque * 4, this.bloque);
      if (calibrar) { dst.fill(0); dst[0] = 0.01; } else dst.set(this.acum[c]);
    }
    const bytes = o + canales * this.bloque * 4;

    const sec = this.secuencia++ >>> 0;
    if (calibrar) { this.secCalib = sec; this.impulsoFrame = this.inicio; this.calib = 'esperando'; }
    this.dv.setUint32(0, sec, true);
    this.dv.setUint16(4, canales, true);
    this.dv.setUint16(6, this.bloque, true);
    this.dv.setUint16(8, nMidi, true);
    this.dv.setUint16(10, this.saltar || calibrar ? 1 : 0, true);
    this.dv.setUint32(12, 0, true);

    const copia = this.peticion.slice(0, bytes);
    this.puerto.postMessage({ tipo: 'bloque', datos: copia }, [copia]);
    this.bloquesEnviados++;
  }

  process(entradas, salidas) {
    if (!this.vivo) return false;
    const ent = entradas[0] || [];
    const sal = salidas[0];

    // 1. Entrada → bloque en curso
    if (this.n === 0) {
      this.inicio = currentFrame;
      if (this.calib === 'pendiente' && this.puerto !== null && this.cebado && currentFrame - this.frameCanal > 0.5 * sampleRate) this.calib = 'enviando';
    }
    for (let c = 0; c < this.entradas; c++) {
      const src = ent[c] || ent[0];
      if (src) this.acum[c].set(src, this.n); else this.acum[c].fill(0, this.n, this.n + CUANTO);
    }
    this.n += CUANTO;
    if (this.n >= this.bloque) {
      this.enviar();
      this.n = 0;
    }

    // 2. Anillo → salida
    let disponible = this.esc - this.lec;
    if (!this.cebado && disponible >= this.colchon) this.cebado = true;
    if (this.cebado && disponible >= CUANTO) {
      const l = this.lec;
      for (let k = 0; k < sal.length; k++) {
        const origen = this.anillo[Math.min(k, 1)];
        const dst = sal[k];
        for (let i = 0; i < CUANTO; i++) dst[i] = origen[(l + i) & this.mascara];
      }
      if (this.calib === 'enSalida') {
        if (this.posImpulso >= l && this.posImpulso < l + CUANTO) this.terminarCalibracion(currentFrame + (this.posImpulso - l) - this.impulsoFrame);
        else if (this.posImpulso < l) this.reintentarCalibracion();   // un descarte se llevó el impulso
      }
      this.lec += CUANTO;
      // Si se ha acumulado demasiado (tras una parada del Worker), se tira lo más viejo para que el retardo no crezca.
      const exceso = (this.esc - this.lec) - (this.colchon + 2 * this.bloque);
      if (exceso > 0) { this.lec += exceso; this.descartes++; }
    } else {
      for (let k = 0; k < sal.length; k++) sal[k].fill(0);
      if (this.cebado) {
        this.cebado = false;
        this.cortes++;
        // Un corte indica que el colchón se queda corto para este ordenador: se sube un bloque (más retardo, pero sin huecos).
        if (this.colchon + this.bloque <= this.colchonMax) {
          this.colchon += this.bloque;
          this.port.postMessage({ tipo: 'colchon', muestras: this.colchon });
        }
      }
    }

    // 3. Estadísticas, una vez por segundo
    this.contador += CUANTO;
    if (this.contador >= sampleRate) {
      this.contador = 0;
      this.port.postMessage({
        tipo: 'stats', cortes: this.cortes, perdidos: this.perdidos, descartes: this.descartes,
        enviados: this.bloquesEnviados, recibidos: this.bloquesRecibidos,
        guardado: this.esc - this.lec, latenciaPlugin: this.latenciaPlugin, carga: this.carga, banderas: this.banderas,
      });
    }
    if ((this.calib === 'esperando' || this.calib === 'enSalida') && currentFrame - this.impulsoFrame > 2 * sampleRate) this.reintentarCalibracion();
    return true;
  }

  terminarCalibracion(muestras) {
    this.calib = 'hecha';
    this.port.postMessage({ tipo: 'calibrada', muestras });
  }

  reintentarCalibracion() {
    if (++this.intentos >= 3) { this.terminarCalibracion(-1); return; }
    this.calib = 'pendiente';
    this.frameCanal = currentFrame;
  }
}

registerProcessor('synz-puente', ProcesadorPuente);
