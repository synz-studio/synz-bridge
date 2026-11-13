> Notas de desarrollo traídas del repositorio del estudio. Las rutas con `puente/` y los nombres `SamplerModule.jsx`, `play/` o `fuentes/` se refieren al repositorio privado del estudio; en este repositorio el contenido de `puente/` es la raíz.

# SYNZ Bridge — plugins del ordenador dentro del estudio

Un navegador no puede cargar un plugin VST3 o AU. Este programa sí: es una aplicación pequeña que se instala en el
ordenador, **busca los plugins que ya tiene el usuario**, los abre y deja que el estudio (en el navegador) les mande
notas y audio y reciba el sonido de vuelta. Todo ocurre dentro del mismo ordenador: nada sale a Internet.

    navegador (synz.studio)  ←— WebSocket en 127.0.0.1 —→  SYNZ Bridge  ←→  plugin VST3 / AU

Estado: **prototipo probado solo en Linux** (ver «Qué no está probado»). El estudio lo usa desde el módulo **PLUGIN VST** (solo administrador por ahora).

## Guía rápida para quien lo usa (Windows)

1. **Descarga y abre.** Descomprime el zip y abre `SYNZ Bridge.exe`. No se instala nada. Windows avisará de que el programa no está firmado
   («Windows protegió su PC»): pulsa **Más información › Ejecutar de todos modos**. Se queda abierto en una ventana pequeña (el registro de lo que pasa).
2. **Tus plugins.** Busca solo los VST3 que ya tienes en `C:\Program Files\Common Files\VST3`. Para otra carpeta: botón de la ventana del puente (o `--carpeta-plugins`).
   Un plugin que se cuelga o se cae al cargarlo se aparta solo (queda anotado en el registro) y no tumba el programa.
3. **En el estudio** (Chrome, Edge o Firefox; Safari no deja): con el puente abierto,
   - **instrumentos**: biblioteca › Instrumentos › *Plugin VST* → la tarjeta de abajo trae el selector de plugin, Vol, Pan y cuatro mandos; ampliar (⤢ o doble clic) abre la ventana real del plugin;
   - **efectos**: biblioteca › Track effects › Utility › *Plugin VST*, en cualquier pista o en el máster; su retardo lo compensa la mesa y sale en la exportación;
   - **clic derecho** sobre la tarjeta del instrumento: foto de la ventana, poner una imagen, elegir los mandos, buscar plugins otra vez.
4. **Si el estudio dice «Abre SYNZ Bridge»**: el puente no está abierto (o se cerró). Ábrelo; el estudio lo vuelve a buscar solo cada pocos segundos.
5. **La primera vez**, el puente pregunta si permite a la web del estudio usar tus plugins (solo `synz.studio` entra sin preguntar). Elige «Permitir siempre».
6. **Actualizar**: cierra el puente, descomprime el zip nuevo y ábrelo. Tus ajustes (`%APPDATA%\SYNZ Bridge`: `config.json`, `puente.log`) se conservan. Si el puente es antiguo,
   el estudio lo avisa en el menú del clic derecho y en la tarjeta del efecto.

## Qué hay aquí

| Carpeta | Qué es |
|---|---|
| `PROTOCOLO.md` | El protocolo, completo (control en JSON, audio en binario). Es lo que manda. |
| `fuente/` | El programa nativo (C++ con JUCE): servidor WebSocket propio, escaneo, alojamiento de plugins, ventana. |
| `cliente/` | Lo que carga el navegador: `puente-cliente.js` (API), `puente-worklet.js` (AudioWorklet) y `puente-trabajador.js` (Worker). Sin dependencias y **sin SharedArrayBuffer** (no hacen falta cabeceras COOP/COEP). |
| `pruebas/` | Pruebas automáticas y plugins VST3 de prueba (un sinte, un efecto con retardo real y uno que se cae a propósito). |

## En el estudio

El módulo **PLUGIN VST** (`vstext`, en `SamplerModule.jsx`) conecta con `ws://127.0.0.1:47821`, lista los instrumentos VST3/AU del
ordenador, carga el que se elija, lo toca con el teclado, el MIDI y las pistas MIDI (las notas se adelantan lo que tarda el
conducto), no tiene ventana propia: la tarjeta de abajo lleva cuatro mandos del plugin (elegibles, automatizables) y su foto (el puente fotografía la ventana real, o se pone una imagen); ampliar abre la ventana real del plugin, por delante de todo y con barra de título de SYNZ, y guarda su estado interno en el proyecto. Al quitar el módulo se
suelta el plugin y se cierra la conexión. El cliente viaja dentro de `SamplerModule.jsx` como texto (entre las marcas
`PUENTE-CLIENTE`); **si cambia `puente/cliente/`, hay que ejecutar `node puente/pruebas/sincroniza-estudio.mjs` y recompilar**
(`sincronia.test.mjs` lo vigila).

## Compilar

Hace falta CMake ≥ 3.22 y un compilador de C++17. JUCE 8.0.9 se descarga solo (o `-DJUCE_DIR=/ruta/a/JUCE`).

    cmake -S puente -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release -j

En Linux hacen falta además las bibliotecas de JUCE (`libasound2-dev libfreetype-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libgl-dev`).
Con `-DSYNZ_PUENTE_PRUEBAS=OFF` no se compilan los plugins de prueba.

## Usar

    "SYNZ Bridge" [--puerto N] [--puerto-osc N] [--osc-red] [--permitir-origen https://…] [--carpeta-plugins RUTA] [--sin-ventana] [--sin-preguntar]

Al arrancar escribe `LISTO <puerto>` (puerto 47821 por defecto). Desde el navegador:

```js
import { PuenteCliente } from './puente-cliente.js';
const puente = await PuenteCliente.conectar({ url: 'ws://127.0.0.1:47821' });
const lista = await puente.plugins();                 // [{ id, nombre, fabricante, instrumento, … }]
const inst = await puente.crear(ctx, lista[0].id);    // ctx = AudioContext del estudio
inst.out.connect(ctx.destination);                    // para un efecto, también inst.in
inst.noteOn(60, 0.8, ctx.currentTime + 0.1);          // MIDI con tiempo: cae en la muestra exacta
await inst.esperarCalibracion();                      // mide el retardo real (unos 0,5 s tras conectar)
inst.latenciaMuestras();                              // ya coincide con lo medido
inst.abrirEditor();                                   // ventana propia del plugin
inst.dispose();                                       // suelta el plugin, el Worker y el canal de audio
```

## Sensores por OSC (desde la 1.3.0)

Además de los plugins, el puente recibe **OSC por UDP** (puerto 9000) y lo pasa al estudio como sensores de SONOLAB (ruta
`/sensores`): Max, Pure Data, SuperCollider, TouchOSC, Python… En el estudio, objeto sensor → enchufe → `osc`. Por defecto
solo escucha a este ordenador; `--osc-red` lo abre a la red (TouchOSC en una tableta) y `--puerto-osc N` cambia el puerto
(`-1` lo apaga). Detalle en `PROTOCOLO.md` §5 y en `docs/guidelines/sensores.md`. Prueba: `sensores.test.mjs`.

## Seguridad

- Solo escucha en `127.0.0.1`. Comprueba la cabecera `Host` (contra DNS rebinding) y exige `Origin`.
- Solo `https://synz.studio` y `https://www.synz.studio` entran sin preguntar. Cualquier otra web provoca un diálogo
  «Permitir siempre / Solo esta vez / Denegar»; sin respuesta en 30 s se deniega. (`--sin-preguntar`: deniega sin diálogo.)
- El canal de audio de cada instancia va con un ticket aleatorio, del mismo origen y de un solo canal por instancia.
- Límites: 8 conexiones de control, 32 instancias, 4 MB por trama de control, 512 KB por trama de audio.
- Las instancias son de la conexión que las creó: si se cierra, se destruyen.
- El **escaneo se hace en un proceso aparte** con plazo: un plugin que se cuelga o se cae al cargarlo no tumba el
  programa; queda en una lista negra con el motivo, y se vuelve a probar con `escanear` con `forzar`.
- Un plugin es código nativo ajeno con acceso total al ordenador. El puente no lo aísla del sistema (es lo que hace
  cualquier DAW). Solo carga los que el usuario ya tiene instalados.

## Pruebas

    cd puente/pruebas && npm ci
    EXE_WS_ECO=…/ws-eco node --test ws.test.mjs                                        # servidor WebSocket (21)
    EXE_PUENTE="…/SYNZ Bridge" VST3_DIR=…/vst3-pruebas node --test puente.test.mjs     # plugins reales (18)
    EXE_PUENTE="…/SYNZ Bridge" VST3_DIR=…/vst3-pruebas node --test cliente.test.mjs    # Chromium real (8)
    EXE_PUENTE="…/SYNZ Bridge" VST3_DIR=…/vst3-pruebas node --test estudio.test.mjs    # el estudio compilado (play/) + puente + VST3 (2)
    node --test sincronia.test.mjs                                                      # la copia del cliente en el estudio al día (1)

Las de plugins y navegador usan `xvfb-run` (ventanas de plugins reales) y Chromium de Playwright (`CHROMIUM=ruta` si no
está en `/opt/pw-browsers/chromium`). `node pruebas/medir-latencia.mjs` imprime una tabla de retardos por tamaño de bloque.

Resultado en el entorno de desarrollo: 50 pruebas en verde. Medido con Chromium: MIDI separado exactamente 4800 muestras,
retardo constante (sin deriva), 0 cortes con el hilo principal bloqueado 500 ms en la mayoría de pasadas, ida y vuelta
~1 ms. **El navegador de este entorno tiene paradas esporádicas del hilo de audio** que a veces dan un corte aislado; por
eso las dos pruebas de tiempo exacto se repiten hasta 5 veces (se repite la medición, no se relaja la exigencia).

## Latencia

Bloque de 128 muestras y colchón de 2 bloques por defecto. Retardo medido en el entorno de pruebas: 512 muestras
(11,6 ms a 44,1 kHz) en el conducto + el del plugin. Lo domina el tamaño de callback del dispositivo de audio del
navegador, no el puente. Si el audio se corta, el colchón sube solo un bloque cada vez (hasta 4× el inicial).
**No hay compensación automática del retardo en el estudio todavía** (PDC).

## Arranque y segundo plano (1.2.0) y un solo puente abierto (1.2.1)

- **Icono en la bandeja** (Windows y macOS): clic para abrir la ventana, clic derecho para «Abrir al iniciar sesión (recomendado)» y «Salir». Cerrar la ventana no cierra el programa.
- **Abrir al iniciar sesión**: la primera vez que se abre a mano pregunta una vez. Arranca con `--en-segundo-plano` (sin ventana, solo el icono). En segundo plano apenas consume (medido en Linux: unos 3 ticks de CPU, 30 ms, en 10 s). `--inicio-automatico si|no` lo pone o quita desde la línea de órdenes (Windows: clave Run del usuario; Linux: `~/.config/autostart`; macOS: LaunchAgent en `~/Library/LaunchAgents`, sin probar).
- **Abrirlo desde el estudio**: al ejecutarse registra el enlace `synz-bridge://abrir` (Windows: registro del usuario; Linux: `.desktop` con el esquema; macOS: esquema `synz-bridge` declarado en el Info.plist de la app, sin probar). El aviso del estudio tiene ABRIR, que usa ese enlace, y si en 9 s no aparece pasa a BUSCAR. Si ya había uno abierto, el segundo se va sin avisar.
- **Un solo puente a la vez:** el puerto (47821) solo lo puede tener uno. El instalador cierra antes de instalar la versión de prueba antigua («SYNZ Puente») y cualquier SYNZ Bridge abierto (`taskkill`), y el desinstalador cierra el que esté en marcha. Si el programa arranca en segundo plano (arranque automático, recién instalado, o abierto desde el estudio) y el puerto está ocupado, se va sin decir nada; si lo abres tú a mano, avisa de que ya hay uno abierto o de que puede ser la versión antigua.
- **Aviso de la primera vez** en el estudio: si no hay puente, una sola vez por sesión sale una ventanita que explica qué hace falta y con DESCARGAR (baja el instalador directamente; ver «Instalador y publicación»).

## Instalador y publicación

- **Instalador de Windows** (`puente/instalador/synz-bridge.iss`, Inno Setup 6): lo compila la acción «Puente» y sale como `SYNZ-Bridge-Setup.exe` en el artefacto `SYNZ-Bridge-instalador`. Se instala para el usuario (sin administrador) en `%LocalAppData%\Programs\SYNZ Bridge`, ofrece «abrir al iniciar sesión» (marcado), registra `synz-bridge://` y se desinstala desde Configuración. La propia acción lo instala y desinstala en silencio y comprueba que quedan (y se quitan) el programa, la clave de arranque y el enlace. **Sin firmar**: Windows avisa de «editor desconocido».
- **Publicar** (producción): «Run workflow» de la acción «Puente» con **publicar** marcado (solo con permiso de Pablo). Compila, prueba y sube por FTPS a `synz.studio/descargas/` el instalador, `SYNZ-Bridge-fuente.zip` (el código, para cumplir la AGPL: el repositorio es privado) y `version.json`; guarda la versión anterior como `*.anterior`. Lo hace `herramientas/sube-descarga.py`.
- **Sin página de descarga**: el botón DESCARGAR del aviso del estudio baja `https://synz.studio/descargas/SYNZ-Bridge-Setup.exe` directamente (`window.SYNZ_PUENTE_URL` lo cambia). El código fuente queda en `https://synz.studio/descargas/SYNZ-Bridge-fuente.zip`. En macOS y Linux el botón dice que solo hay de Windows por ahora.

## Icono

`puente/fuente/icono/synz-bridge.svg` y su `.png` de 1024 px (el que usa la compilación): la onda de SYNZ (la de la barra del estudio, `M1 7 C 3 1, 6 1, 9 7 S 15 13, 17 7`, trazo fino, `#6D74F5`) y «VST» debajo, en un cuadrado oscuro. Es el icono del `.exe`, de la ventana y de la bandeja. La ventana y la bandeja lo dibujan con `iconoBridge()` en `Bandeja.h`, con las mismas medidas: si se cambia el SVG hay que cambiar las dos cosas.

## Si un efecto suena raro

`puente/herramientas/diagnostico-efecto-vst.js`: se pega en la consola del navegador (F12) con el efecto cargado y la reproducción parada. Manda un golpe seco por el plugin, graba lo que sale y dice si el plugin añade cola (eco o reverb), cuánto retarda de verdad, si el puente corta audio y qué mandos de ambiente tiene.

## Qué no está probado

- **Windows: compila y pasa las pruebas del servidor y del puente con plugins VST3 reales en GitHub Actions** (workflow `puente.yml`,
  artefacto `SYNZ-Bridge-windows`), pero **no se ha probado en un Windows con interfaz real ni con el estudio delante**.
- **macOS: preparado, ni compilado ni probado.** El trabajo `macos` de `puente.yml` (solo a mano, ×10 de minutos) compila Intel + Apple Silicon, pasa las pruebas y deja `SYNZ-Bridge-mac.zip` (app sin firmar: se abre con clic derecho › Abrir, o Ajustes › Privacidad y seguridad › Abrir igualmente). En Mac la «foto de la ventana» del plugin no existe (el estudio pide una imagen aparte) y la ventana pasa al frente con `makeForegroundProcess`.
- Plugins reales de terceros: solo los de prueba (los tres VST3 de `pruebas/plugins`). Con plugins comerciales puede haber
  sorpresas (editores que piden el hilo principal, iLok/licencias, plugins que tardan en cargar).
- Nada se ha **oído**: se mide con señales de prueba.
- Sin firma de código ni notarización (macOS lo bloqueará; Windows avisará de «editor desconocido»).
- Chrome pide permiso de «acceso a la red local» al abrir `ws://127.0.0.1` desde una página https; el flujo no se ha
  probado en un Chrome con interfaz.
- Safari bloquea `ws://127.0.0.1` desde una página https (contenido mixto); Chrome, Edge y Firefox lo permiten.
- Solo instrumentos: **efectos como inserto** y automatizar parámetros del plugin con curvas del estudio no están hecho todavía.

## Licencias

- **El puente es software libre bajo AGPL-3.0** (`LICENSE`), sin pagar nada: JUCE 8 se ofrece bajo AGPLv3 para proyectos
  de código abierto, y este lo es. La condición: quien reciba el programa tiene derecho a su código fuente (esta carpeta
  entera). El estudio (`play/`, `fuentes/`) **no** se ve afectado: habla con el puente por WebSocket, como dos programas
  distintos, y mantiene su propia licencia.
- Si algún día se quisiera repartir cerrado, habría que comprar licencia de JUCE o reescribir el alojamiento de plugins sobre
  el SDK de VST3 (que es MIT) sin JUCE; el protocolo y el cliente no cambiarían.
- El código de este directorio es nuevo, escrito desde cero a partir del protocolo y de lo aprendido al leer proyectos
  parecidos; no copia código de otros. (`wasmvst`, que se estudió, no tiene licencia: no se ha reutilizado ni una línea.)
- Dependencias de las pruebas (no se distribuyen): `ws` (MIT), `playwright-core` (Apache-2.0).
- Solo se cargan plugins que el usuario ya tiene. No se distribuye ningún plugin ni audio.
