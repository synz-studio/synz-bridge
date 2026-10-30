// Grabador de audio para las pruebas: guarda lo que le llega, con el `currentFrame` donde empezó.
class Captura extends AudioWorkletProcessor {
  constructor() {
    super();
    this.grabando = false; this.inicio = 0; this.c0 = []; this.c1 = [];
    this.port.onmessage = (e) => {
      if (e.data.tipo === 'inicio') { this.grabando = true; this.inicio = -1; this.c0 = []; this.c1 = []; }
      else if (e.data.tipo === 'fin') {
        this.grabando = false;
        const a = Float32Array.from(this.c0.flatMap((x) => Array.from(x)));
        const b = Float32Array.from(this.c1.flatMap((x) => Array.from(x)));
        this.port.postMessage({ inicio: this.inicio, canal0: a, canal1: b }, [a.buffer, b.buffer]);
      }
    };
  }
  process(entradas) {
    const e = entradas[0];
    if (this.grabando && e && e.length) {
      if (this.inicio < 0) this.inicio = currentFrame;
      this.c0.push(new Float32Array(e[0]));
      this.c1.push(new Float32Array(e[1] || e[0]));
    }
    return true;
  }
}
registerProcessor('captura', Captura);
