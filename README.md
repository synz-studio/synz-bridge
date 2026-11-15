# SYNZ Bridge

Programa gratuito y de código abierto (**AGPL-3.0**) que lleva los plugins **VST3** (y **AU** en macOS) de tu ordenador
dentro de [SYNZ Studio](https://synz.studio), el estudio de música que funciona en el navegador.

Un navegador no puede cargar plugins nativos. SYNZ Bridge corre en tu ordenador, los aloja, y habla con el estudio
por WebSocket **solo en `127.0.0.1:47821`** (nada sale de tu máquina). El estudio le manda notas y audio, y el puente
devuelve el sonido del plugin.

## Descargar

- **Windows:** [`SYNZ-Bridge-Setup.exe`](https://synz.studio/descargas/SYNZ-Bridge-Setup.exe). Sin firma de código por ahora:
  Windows puede avisar de «editor desconocido» (Más información › Ejecutar de todos modos).
- **macOS:** en preparación.

## Sensores por OSC (desde la 1.3.0)

Además de los plugins, el puente recibe **OSC por UDP** en el puerto **9000** (Max, Pure Data, SuperCollider, TouchOSC,
Processing, Python…) y lo pasa al estudio como sensores de SONOLAB. En el estudio: objeto *sensor* → enchufe → `osc`.

    /synz/sensor/<id>/<salida> 0.5        un valor
    /synz/sensor/<id> 0.1 0.2 0.3         varios, por orden
    /synz/rango/<id>/<salida> 0 1023      su rango (el estudio lo pasa a 0..1)
    /1/fader1 0.7                         cualquier otra dirección va al sensor «OSC» (TouchOSC tal cual)

Por defecto solo escucha a este ordenador; `--osc-red` lo abre a la red (TouchOSC en una tableta), `--puerto-osc N` cambia el
puerto y `--puerto-osc -1` lo apaga. Detalle en [`PROTOCOLO.md`](PROTOCOLO.md) §5.

## Entradas de la tarjeta (desde la 1.4.0)

El navegador solo deja abrir dos entradas de una tarjeta de sonido. El puente la abre con todas las que hagan falta (hasta 32) y se las
da al estudio, absorbiendo la diferencia entre el reloj de la tarjeta y el del navegador. En el estudio: el selector de entrada de una
pista › «Más entradas con SYNZ Bridge…». Sirve, por ejemplo, para que un ordenador maestro reciba por cable a varios músicos. Detalle en
[`PROTOCOLO.md`](PROTOCOLO.md) §6.

## Compilar

Hace falta CMake 3.22 o superior y un compilador de C++17 (MSVC, Clang o GCC). JUCE 8.0.9 se descarga solo.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

El programa queda en `build/fuente/synz-puente_artefacts/Release/`.

## Pruebas

    cd pruebas && npm ci
    EXE_WS_ECO=…/ws-eco node --test ws.test.mjs                                  # servidor WebSocket
    EXE_PUENTE="…/SYNZ Bridge" VST3_DIR=…/vst3-pruebas node --test puente.test.mjs   # puente con plugins VST3 de prueba
    EXE_PUENTE="…/SYNZ Bridge" node humo.mjs                                     # arranca, responde y escanea
    EXE_PUENTE="…/SYNZ Bridge" node --test sensores.test.mjs                     # sensores por OSC
    EXE_PUENTE="…/SYNZ Bridge" node --test entradas.test.mjs                     # entradas de la tarjeta (con --entrada-prueba)

## Más información

- [`PROTOCOLO.md`](PROTOCOLO.md): el protocolo entre el estudio y el puente.
- [`docs/NOTAS-DE-DESARROLLO.md`](docs/NOTAS-DE-DESARROLLO.md): decisiones, estado y trampas conocidas.

## Licencia

AGPL-3.0 (ver [`LICENSE`](LICENSE)). JUCE se usa bajo su licencia AGPLv3.
