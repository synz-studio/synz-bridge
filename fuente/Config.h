// Ajustes del usuario que se guardan entre sesiones, en la carpeta de datos del programa.
//   config.json          orígenes que se aceptaron «para siempre», carpetas de plugins añadidas y "oscRed" (OSC desde la red)
//   cache-plugins.xml    qué plugins hay y de cuándo es cada archivo (para no volver a mirarlos)
//   lista-negra.xml      plugins que colgaron o se cayeron al escanearlos
//   puente.log           registro de lo que ha pasado
#pragma once
#include <juce_core/juce_core.h>
#include <mutex>

namespace synz
{
class Config
{
public:
    explicit Config (juce::File carpetaDatos) : carpeta (std::move (carpetaDatos))
    {
        carpeta.createDirectory();
        cargar();
    }

    juce::File dir() const { return carpeta; }
    juce::File archivoCache() const { return carpeta.getChildFile ("cache-plugins.xml"); }
    juce::File archivoListaNegra() const { return carpeta.getChildFile ("lista-negra.xml"); }
    juce::File archivoRegistro() const { return carpeta.getChildFile ("puente.log"); }

    /** Orígenes que no preguntan nunca: los de SYNZ y los que el usuario aceptó «siempre». */
    bool origenSiempre (const juce::String& origen) const
    {
        std::lock_guard<std::mutex> g (m);
        return origen == "https://synz.studio" || origen == "https://www.synz.studio" || guardados.contains (origen);
    }

    void permitirSiempre (const juce::String& origen)
    {
        {
            std::lock_guard<std::mutex> g (m);
            guardados.addIfNotAlreadyThere (origen);
        }
        guardar();
    }

    juce::StringArray carpetasExtra() const
    {
        std::lock_guard<std::mutex> g (m);
        return carpetas;
    }

    bool oscRed() const
    {
        std::lock_guard<std::mutex> g (m);
        return osc;
    }

    void agregarCarpeta (const juce::String& ruta)
    {
        {
            std::lock_guard<std::mutex> g (m);
            carpetas.addIfNotAlreadyThere (ruta);
        }
        guardar();
    }

private:
    void cargar()
    {
        const auto f = carpeta.getChildFile ("config.json");
        if (! f.existsAsFile())
            return;
        const auto v = juce::JSON::parse (f);
        if (auto* o = v.getDynamicObject())
        {
            std::lock_guard<std::mutex> g (m);
            if (auto* a = o->getProperty ("origenes").getArray())
                for (auto& x : *a) guardados.addIfNotAlreadyThere (x.toString());
            if (auto* a = o->getProperty ("carpetas").getArray())
                for (auto& x : *a) carpetas.addIfNotAlreadyThere (x.toString());
            osc = (bool) o->getProperty ("oscRed");
        }
    }

    void guardar() const
    {
        auto* o = new juce::DynamicObject();
        juce::Array<juce::var> a, c;
        {
            std::lock_guard<std::mutex> g (m);
            for (auto& x : guardados) a.add (x);
            for (auto& x : carpetas) c.add (x);
        }
        o->setProperty ("origenes", a);
        o->setProperty ("carpetas", c);
        {
            std::lock_guard<std::mutex> g (m);
            if (osc) o->setProperty ("oscRed", true);
        }
        carpeta.getChildFile ("config.json").replaceWithText (juce::JSON::toString (juce::var (o)));
    }

    const juce::File carpeta;
    mutable std::mutex m;
    juce::StringArray guardados, carpetas;
    bool osc = false;
};
} // namespace synz
