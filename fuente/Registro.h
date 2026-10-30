// Registro de lo que pasa en el puente: a un archivo (con rotación a 1 MB) y a quien quiera mostrarlo en pantalla.
#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <mutex>

namespace synz
{
class Registro
{
public:
    explicit Registro (juce::File archivoLog) : archivo (std::move (archivoLog)) {}

    std::function<void (const juce::String&)> alEscribir;   // se llama en el hilo que escribe

    void escribir (const juce::String& texto)
    {
        const auto linea = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S  ") + texto;
        {
            std::lock_guard<std::mutex> g (m);
            if (archivo != juce::File() && archivo.getParentDirectory().exists())
            {
                if (archivo.getSize() > 1024 * 1024)
                    archivo.moveFileTo (archivo.withFileExtension ("log.1"));
                archivo.appendText (linea + "\n");
            }
        }
        if (alEscribir)
            alEscribir (linea);
    }

private:
    std::mutex m;
    juce::File archivo;
};
} // namespace synz
