// Worker del puente: lleva los bloques de audio entre el AudioWorklet (por un MessagePort directo) y el WebSocket
// de audio de su instancia. Así ni el hilo de audio toca la red ni el hilo principal está en medio.
let ws = null;
let puerto = null;
let cerrando = false;
const enviadoEn = new Map();     // secuencia → instante de envío, para medir la ida y vuelta
let rtt = 0;
let rttMax = 0;
let contador = 0;

self.onmessage = (e) => {
  const m = e.data;
  if (m.tipo === 'iniciar') {
    puerto = m.puerto;
    puerto.onmessage = (ev) => { if (ev.data.tipo === 'bloque') enviar(ev.data.datos); };
    abrir(m.url);
  } else if (m.tipo === 'cerrar') {
    cerrando = true;
    if (ws) ws.close(1000);
    else self.postMessage({ tipo: 'cerrado', codigo: 1000 });
  }
};

function abrir(url) {
  ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => self.postMessage({ tipo: 'abierto' });
  ws.onerror = () => { /* el motivo llega con onclose */ };
  ws.onclose = (ev) => {
    const codigo = ev.code;
    ws = null;
    self.postMessage({ tipo: 'cerrado', codigo, esperado: cerrando });
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') return;
    const b = ev.data;
    if (b.byteLength >= 4) {
      const sec = new DataView(b).getUint32(0, true);
      const t0 = enviadoEn.get(sec);
      if (t0 !== undefined) {
        enviadoEn.delete(sec);
        const dt = performance.now() - t0;
        rtt = rtt === 0 ? dt : rtt * 0.95 + dt * 0.05;
        if (dt > rttMax) rttMax = dt;
      }
    }
    puerto.postMessage({ tipo: 'respuesta', datos: b }, [b]);
    if (++contador % 200 === 0) {
      self.postMessage({ tipo: 'rtt', media: rtt, maximo: rttMax });
      rttMax = 0;
      if (enviadoEn.size > 64) enviadoEn.clear();   // respuestas que no llegaron: no se acumulan
    }
  };
}

function enviar(buf) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;     // sin conexión el bloque se pierde: el worklet seguirá en silencio
  enviadoEn.set(new DataView(buf).getUint32(0, true), performance.now());
  ws.send(buf);
}
