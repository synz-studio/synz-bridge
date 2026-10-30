// Busca plugins en las carpetas habituales (más las que añada el usuario). Cada archivo se mira en un PROCESO
// APARTE (el propio programa lanzado con `--escanear-uno`), con plazo máximo: un plugin que se cuelga o se cae
// no tumba el puente, se anota en la lista negra y se sigue.
#pragma once
#include "Config.h"
#include "Registro.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <map>
#include <vector>

namespace synz
{
struct PluginFallido
{
    juce::String archivo, motivo;
};

class Escaner : private juce::Thread
{
public:
    Escaner (Config& c, Registro& r, juce::AudioPluginFormatManager& f, juce::File programa);
    ~Escaner() override;

    /** Empieza un escaneo en segundo plano (si ya hay uno en marcha, no hace nada). `forzar` ignora la caché y la lista negra. */
    void iniciar (bool forzar);
    bool escaneando() const { return isThreadRunning(); }

    std::vector<juce::PluginDescription> lista() const;
    bool buscar (const juce::String& id, juce::PluginDescription& destino) const;

    std::function<void (const juce::String& actual, double progreso)> alProgreso;
    std::function<void (int total, const std::vector<PluginFallido>& fallidos)> alTerminar;

    /** Plazo máximo para mirar un archivo. */
    int plazoMs = 30000;
    /** Carpetas adicionales solo para esta ejecución (por ejemplo, las de las pruebas). */
    juce::StringArray carpetasTemporales;

private:
    struct Entrada
    {
        int64_t modificado = 0;
        std::vector<juce::PluginDescription> plugins;
    };

    void run() override;
    bool escanearUno (const juce::String& formato, const juce::String& archivo, std::vector<juce::PluginDescription>& destino, juce::String& motivo);
    void cargarCache();
    void guardarCache() const;
    void cargarListaNegra();
    void guardarListaNegra() const;

    Config& config;
    Registro& registro;
    juce::AudioPluginFormatManager& formatos;
    const juce::File programa;

    mutable std::mutex m;
    std::map<juce::String, Entrada> cache;                // clave: «formato|archivo»
    std::map<juce::String, int64_t> listaNegra;           // clave → fecha del archivo cuando falló
    std::vector<juce::PluginDescription> actuales;
    bool forzando = false;
};

/** Modo hijo: mira un archivo y escribe el resultado en la salida estándar. Devuelve el código de salida. */
int escanearArchivoYEscribir (const juce::String& formato, const juce::String& archivo);
} // namespace synz
