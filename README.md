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

## Más información

- [`PROTOCOLO.md`](PROTOCOLO.md): el protocolo entre el estudio y el puente.
- [`docs/NOTAS-DE-DESARROLLO.md`](docs/NOTAS-DE-DESARROLLO.md): decisiones, estado y trampas conocidas.

## Licencia

AGPL-3.0 (ver [`LICENSE`](LICENSE)). JUCE se usa bajo su licencia AGPLv3.
