# Protocolo del puente de plugins nativos (versión 1)

El puente es un programa de escritorio que aloja plugins VST3 (y AU en Mac) y habla con el estudio por
WebSocket, solo en `127.0.0.1`. Este documento es la referencia: el programa (`fuente/`) y el cliente del
navegador (`cliente/`) se escriben contra él.

Todos los números binarios son **little-endian**. El audio va en **float32, plano** (primero todas las muestras
del canal 0, luego las del 1…).

## 1. Conexión y seguridad

- Dirección por defecto: `ws://127.0.0.1:47821`. Se puede cambiar con `--puerto`.
- Solo escucha en el bucle local. Nunca en `0.0.0.0`.
- **Cabecera `Host`**: debe ser `127.0.0.1:<puerto>`, `localhost:<puerto>` o `[::1]:<puerto>`. Cualquier otra se
  rechaza con 403 (protege contra el «DNS rebinding»: una web ajena que apunte su nombre a 127.0.0.1).
- **Cabecera `Origin`**: obligatoria. Debe estar en la lista permitida. Por defecto: `https://synz.studio` y
  `https://www.synz.studio`. Un origen que no esté en la lista **pregunta al usuario en la ventana del puente**
  («Permitir una vez», «Permitir siempre», «Denegar»); sin respuesta en 30 s se deniega. `--permitir-origen <origen>`
  (repetible) añade orígenes sin preguntar, para pruebas y desarrollo.
- `GET /` (sin `Upgrade`) responde `200` con el texto `synz-puente 1` y nada más. Sin cabeceras CORS.
- Límites: 8 conexiones de control a la vez, 32 instancias de plugin en total, trama de control ≤ 4 MB, trama de
  audio ≤ 512 KB, cabeceras HTTP ≤ 8 KB, handshake en ≤ 5 s. Lo que se salga cierra la conexión.
- Las rutas son `/control` y `/audio/<instancia>`. Otra ruta responde 404.

## 2. Canal de control: `/control`

Mensajes de texto JSON, UTF-8.

Petición: `{ "id": <número>, "op": "<operación>", ...argumentos }`
Respuesta: `{ "id": <mismo>, "ok": true, ...campos }` o `{ "id": <mismo>, "ok": false, "codigo": "<código>", "error": "<texto>" }`

Códigos de error: `peticion_invalida`, `operacion_desconocida`, `plugin_desconocido`, `instancia_desconocida`,
`limite`, `fallo_plugin`, `no_disponible`.

| `op` | Argumentos | Respuesta |
|---|---|---|
| `hola` | `protocolo` (el que habla el cliente) | `protocolo`, `puente` (versión, p. ej. `1.1.0`), `sistema` (`mac`, `windows`, `linux`), `formatos` (p. ej. `["VST3"]`), `plugins` (cuántos), `escaneando`, `instancias` (cuántas hay abiertas ahora), `capacidades` (lista de lo que sabe hacer: `capturarEditor`, `escalarEditor`, `ventanaSynz`; un puente antiguo no las trae) |
| `listar` | — | `plugins`: `[{ id, nombre, fabricante, version, formato, categoria, instrumento, entradas, salidas }]` |
| `escanear` | `forzar` (opcional, vuelve a mirar todo) | `iniciado`. Después llegan los eventos `escaneo` y `escaneoTerminado` |
| `crear` | `plugin` (id), `frecuencia` (Hz), `bloqueMax` (muestras, ≤ 2048), `entradas`, `salidas` (opcionales, 0–8) | `instancia`, `ticket`, `nombre`, `instrumento`, `entradas`, `salidas`, `latencia` (muestras), `editor` (booleano), `parametros`: `[{ i, id, nombre, valor, porDefecto, etiqueta, pasos, automatizable }]` |
| `destruir` | `instancia` | — |
| `abrirEditor` | `instancia`, `escala` (opcional, 0 a 3: zoom de la ventana; 1 = normal) | — (la ventana del plugin se abre en el escritorio, con barra de título de SYNZ y por encima de las demás un momento) |
| `escalarEditor` | `instancia`, `escala` (0,25 a 3) | — (zoom de la ventana abierta; lo respetan los plugins que admiten escala; si no está abierta, `no_disponible`) |
| `cerrarEditor` | `instancia` | — |
| `capturarEditor` | `instancia` | `imagen` (JPEG en base64, 480 de ancho como mucho), `tipo`, `ancho`, `alto`. La ventana tiene que estar abierta; si no, `no_disponible`. En Windows se pide al sistema la imagen real de la ventana del plugin; en el resto, lo que pinta JUCE (puede salir vacío con plugins que dibujan en una ventana propia) |
| `leerEstado` | `instancia` | `estado`: base64 |
| `ponerEstado` | `instancia`, `estado` (base64) | `parametros`: lista actualizada |
| `leerParametros` | `instancia` | `valores`: `[valor, …]` (0 a 1, en el orden de `parametros`) |
| `ponerParametro` | `instancia`, `i`, `valor` (0 a 1) | — |

`valor` y `porDefecto` van **normalizados (0 a 1)**. `etiqueta` es el texto de unidad («dB», «Hz»). `pasos` es el
número de valores discretos, o 0 si es continuo.

### Eventos (los manda el puente sin que se pidan)

| `evento` | Campos |
|---|---|
| `escaneo` | `actual` (nombre del archivo), `progreso` (0 a 1) |
| `escaneoTerminado` | `total`, `fallidos`: `[{ archivo, motivo }]` |
| `parametro` | `instancia`, `i`, `valor` — el usuario movió algo en la ventana del plugin. Agrupados: como mucho 30 por segundo y parámetro |
| `editorCerrado` | `instancia` |
| `instanciaCaida` | `instancia`, `motivo` — el plugin lanzó una excepción al procesar; se queda en silencio hasta que se destruya |

### Vida de las instancias

Cada instancia pertenece a la conexión de control que la creó. Si esa conexión se cierra, el puente destruye sus
instancias y cierra su ventana de editor.

## 3. Canal de audio: `/audio/<instancia>?t=<ticket>`

Un canal por instancia. Hay que abrirlo con el `ticket` que devolvió `crear` y desde el mismo origen que la
conexión de control; si no, 403. Solo se acepta un canal de audio por instancia.

Mensajes binarios. El navegador manda **una petición por bloque** y el puente responde con **una respuesta**, en el
mismo orden. El reloj de audio del navegador manda: el puente nunca procesa por su cuenta.

### Petición (navegador → puente)

```
offset  tipo  campo
0       u32   secuencia            (el navegador la numera; el puente la copia en la respuesta)
4       u16   canales              (canales de entrada que vienen en este mensaje, 0 a 8)
6       u16   muestras             (tamaño del bloque, 1 a bloqueMax)
8       u16   nMidi                (eventos MIDI, 0 a 256)
10      u16   banderas             (bit 0: saltar el plugin y devolver la entrada tal cual)
12      u32   reservado            (0)
16      …     nMidi × evento MIDI de 8 bytes
        …     canales × muestras × f32
```

Evento MIDI (8 bytes):

```
0  u16  desplazamiento   (muestra dentro del bloque, 0 a muestras-1: la nota empieza exactamente ahí)
2  u8   estado           (0x80 a 0xEF; los mensajes de sistema no se aceptan)
3  u8   dato1
4  u8   dato2
5  u8   longitud         (1, 2 o 3 bytes útiles)
6  u16  reservado        (0)
```

Los eventos van ordenados por `desplazamiento`; si no, el puente los ordena.

### Respuesta (puente → navegador)

```
0   u32  secuencia
4   u16  canales           (canales de salida, los de la instancia)
6   u16  muestras          (las mismas que la petición)
8   u32  latencia          (retardo propio del plugin, en muestras, valor de este instante)
12  u16  carga             (tiempo que tardó el bloque, en centésimas de porcentaje del tiempo disponible: 10000 = 100 %)
14  u16  banderas          (bit 0: el bloque llegó tarde y se procesó igualmente; bit 1: el plugin está caído y la salida es silencio)
16  …    canales × muestras × f32
```

## 4. Cómo calcular la latencia total en el estudio

`latencia total = bloque + colchón + viaje de ida y vuelta + latencia del plugin`

- `bloque`: muestras de cada petición (el cliente usa 128 o 256).
- `colchón`: muestras que el cliente guarda antes de empezar a reproducir (absorbe la irregularidad de la red local).
- `viaje`: lo mide el cliente (ida y vuelta de cada bloque).
- `latencia del plugin`: campo `latencia` de la respuesta.

El estudio usa esa suma para compensar los retardos (PDC) y para colocar bien lo que se graba.

## 5. Sensores por OSC: `/sensores` (desde la 1.3.0)

Para los sensores del estudio (`docs/guidelines/sensores.md`, fase 3). El puente escucha **OSC por UDP** en el puerto 9000
(`--puerto-osc N`; `-1` = sin OSC; `0` = que lo elija el sistema) y lo pasa al estudio por la ruta WebSocket `/sensores`.
Por defecto el UDP solo escucha en `127.0.0.1`; con `--osc-red` (o `"oscRed": true` en `config.json`), en todas las
interfaces, para que TouchOSC u otro aparato de la misma red mande al ordenador. La ruta `/sensores` pide un origen
permitido, como `/control`. Al arrancar, después de `LISTO <puerto>`, el puente escribe `OSC <puerto>` (`-1` si no lo abrió),
y `hola` lleva `"osc": <puerto>` y la capacidad `sensoresOsc`.

Direcciones OSC (argumentos `i` o `f`; lo demás se ignora; los bundles se abren):

| Dirección | Qué hace |
|---|---|
| `/synz/sensor/<id>/<salida> v` | el valor de una salida de un sensor |
| `/synz/sensor/<id> v1 v2 …` | varias salidas a la vez, por orden (`s1`, `s2`…) |
| `/synz/rango/<id>/<salida> min max` | el rango de esa salida (el estudio lo pasa a 0..1); sin él, se espera 0..1 |
| cualquier otra, p. ej. `/1/fader1 v` | va al sensor `osc`: una salida por dirección (y por argumento: `accxyz-1`, `-2`…), por orden de llegada |

Los ids y las salidas se limpian a `a-z`, `0-9`, `-` y `_` (32 y 24 caracteres). Hasta 8 sensores y 8 salidas por sensor;
4 conexiones a `/sensores` a la vez.

Lo que manda el puente por `/sensores` es el JSON de un programa del laboratorio (el mismo que la fase 2), en **una lista por
mensaje**, como mucho cada 8 ms, solo con lo que cambió:

```json
[{ "synz": "sensor", "id": "lab-flex", "nombre": { "es": "lab-flex", "en": "lab-flex" },
   "salidas": [{ "id": "indice", "l": "indic", "min": 0, "max": 1, "h": { "es": "OSC: /synz/sensor/lab-flex/indice", "en": "…" } }] },
 { "id": "lab-flex", "v": { "indice": 0.25 } }]
```

El alta se repite cuando un sensor gana una salida o un rango, y para todos cuando se conecta alguien nuevo. Lo que mande el
estudio por esta ruta se lee y se descarta.
