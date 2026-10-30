// Diagnóstico de un «Plugin VST» puesto como efecto (God Particle, etc.).
// Se pega entero en la consola del navegador (F12 → Consola) con el estudio abierto, la reproducción PARADA y el efecto cargado.
// No toca nada del proyecto: manda un golpe seco por el plugin, graba lo que sale y cuenta cuánta «cola» (eco o reverb) le añade.
(async () => {
  const regs = Object.entries(window.__synzVstIns || {}).filter(([, e]) => e && e.inst);
  if (!regs.length) { console.log("No hay ningún Plugin VST cargado como efecto."); return; }
  const db = (x) => (x > 1e-9 ? Math.round(20 * Math.log10(x) * 10) / 10 : -120);
  for (const [clave, est] of regs) {
    const inst = est.inst, ctx = inst.ctx || window.__synzAudioCtx, fs = ctx.sampleRate;
    console.log("%c── " + inst.nombre + " (" + clave + ") ──", "font-weight:bold");
    await ctx.resume();
    const antes = { ...inst.estado };
    // 1. Un golpe seco (0,5 de nivel, una muestra) en 0,2 s, y 3 s de silencio detrás.
    const buf = ctx.createBuffer(2, Math.round(fs * 3.4), fs);
    for (let c = 0; c < 2; c++) buf.getChannelData(c)[Math.round(fs * 0.2)] = 0.5;
    const src = ctx.createBufferSource(); src.buffer = buf;
    // 2. Se graba lo que sale del plugin.
    const sp = ctx.createScriptProcessor(2048, 2, 2), mudo = ctx.createGain(); mudo.gain.value = 0;
    const L = [], R = [];
    sp.onaudioprocess = (e) => { L.push(new Float32Array(e.inputBuffer.getChannelData(0))); R.push(new Float32Array(e.inputBuffer.getChannelData(1))); };
    inst.out.connect(sp); sp.connect(mudo); mudo.connect(ctx.destination);
    src.connect(inst.in); src.start();
    await new Promise((r) => setTimeout(r, 3800));
    try { src.disconnect(); inst.out.disconnect(sp); sp.disconnect(); mudo.disconnect(); } catch (e) {}
    const plano = (A) => { const o = new Float32Array(A.length * 2048); A.forEach((b, i) => o.set(b, i * 2048)); return o; };
    const l = plano(L), r = plano(R);
    // 3. Dónde llega el golpe y cuánto queda después.
    let pico = 0, pos = 0;
    for (let i = 0; i < l.length; i++) { const v = Math.max(Math.abs(l[i]), Math.abs(r[i])); if (v > pico) { pico = v; pos = i; } }
    const tramo = (a, b) => { // nivel máximo entre a y b ms después del golpe, en dB respecto al golpe
      let m = 0; const i0 = pos + Math.round(fs * a / 1000), i1 = Math.min(l.length, pos + Math.round(fs * b / 1000));
      for (let i = i0; i < i1; i++) m = Math.max(m, Math.abs(l[i]), Math.abs(r[i]));
      return pico > 0 ? db(m / pico) : -120;
    };
    const tabla = [["5–20 ms", tramo(5, 20)], ["20–60 ms", tramo(20, 60)], ["60–200 ms", tramo(60, 200)], ["200–600 ms", tramo(200, 600)], ["600–1500 ms", tramo(600, 1500)]];
    console.log("Retardo del golpe: " + Math.round(pos / fs * 1000 - 200) + " ms (el estudio cree que son " + Math.round(inst.latenciaMs()) + " ms) · nivel del golpe a la salida: " + db(pico) + " dB (entró a −6 dB)");
    console.table(tabla.map(([t, v]) => ({ tramo: t, "nivel respecto al golpe (dB)": v })));
    const cola = Math.max(tabla[2][1], tabla[3][1]);
    console.log(pico === 0 ? "El plugin no devolvió nada (¿apagado o con la salida muda?)."
      : cola > -35 ? "→ A un golpe seco el plugin le añade una COLA audible (eco/reverb) de " + cola + " dB: viene del propio plugin (un módulo de ambiente/espacio, o el preset). Mira sus mandos de más abajo."
      : "→ Sin cola: ante un golpe seco, el plugin no añade reverb ni eco. Si en la mezcla se oye, el motivo es otro (revisa los cortes de abajo o el nivel de entrada).");
    // 4. Si el audio se corta: cortes y descartes de este rato.
    const ahora = inst.estado;
    console.log("Cortes de audio en el puente: " + (ahora.cortes - antes.cortes) + " · perdidos: " + (ahora.perdidos - antes.perdidos) + " · descartes: " + (ahora.descartes - antes.descartes) + " · carga del puente: " + ahora.carga + " · colchón: " + inst.colchon + " muestras");
    // 5. Los mandos del plugin que suenan a ambiente.
    const re = /reverb|room|space|spatial|ambien|depth|air|wet|mix|decay|size|width|stereo|hall|plate|echo|delay/i;
    const vals = await inst.leerParametros().catch(() => []);
    const mandos = est.params.map((q, k) => ({ mando: q.nombre, valor: Math.round((vals[k] ?? q.valor) * 1000) / 1000 })).filter((m) => re.test(m.mando));
    if (mandos.length) { console.log("Mandos del plugin que pueden dar ambiente (valor de 0 a 1):"); console.table(mandos); }
    else console.log("El plugin no enseña mandos con nombre de ambiente/mezcla al estudio (mira su ventana).");
  }
})();
