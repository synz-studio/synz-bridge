#include "Escaner.h"
#if JUCE_MAC
 #include <dlfcn.h>
#endif
#include <algorithm>
#include "Texto.h"
#include <iostream>

namespace synz
{
Escaner::Escaner (Config& c, Registro& r, juce::AudioPluginFormatManager& f, juce::File prog)
    : juce::Thread ("escaner-plugins"), config (c), registro (r), formatos (f), programa (std::move (prog))
{
    cargarCache();
    cargarListaNegra();
    // Lo que ya se sabe está disponible desde el primer momento, sin esperar a un escaneo.
    for (auto& [clave, entrada] : cache)
        actuales.insert (actuales.end(), entrada.plugins.begin(), entrada.plugins.end());
}

Escaner::~Escaner()
{
    stopThread (5000);
}

void Escaner::iniciar (bool forzar)
{
    if (isThreadRunning())
        return;
    forzando = forzar;
    startThread (Priority::low);
}

std::vector<juce::PluginDescription> Escaner::lista() const
{
    std::lock_guard<std::mutex> g (m);
    return actuales;
}

bool Escaner::buscar (const juce::String& id, juce::PluginDescription& destino) const
{
    std::lock_guard<std::mutex> g (m);
    for (auto& d : actuales)
        if (d.createIdentifierString() == id)
        {
            destino = d;
            return true;
        }
    return false;
}

//==============================================================================
void Escaner::cargarCache()
{
    if (auto xml = juce::XmlDocument::parse (config.archivoCache()))
        for (auto* e : xml->getChildWithTagNameIterator ("ENTRADA"))
        {
            Entrada entrada;
            entrada.modificado = e->getStringAttribute ("mod").getLargeIntValue();
            for (auto* p : e->getChildWithTagNameIterator ("PLUGIN"))
            {
                juce::PluginDescription d;
                if (d.loadFromXml (*p))
                    entrada.plugins.push_back (d);
            }
            cache[e->getStringAttribute ("clave")] = std::move (entrada);
        }
}

void Escaner::guardarCache() const
{
    juce::XmlElement raiz ("CACHE");
    {
        std::lock_guard<std::mutex> g (m);
        for (auto& [clave, entrada] : cache)
        {
            auto* e = raiz.createNewChildElement ("ENTRADA");
            e->setAttribute ("clave", clave);
            e->setAttribute ("mod", juce::String (entrada.modificado));
            for (auto& d : entrada.plugins)
                e->addChildElement (d.createXml().release());
        }
    }
    raiz.writeTo (config.archivoCache());
}

void Escaner::cargarListaNegra()
{
    if (auto xml = juce::XmlDocument::parse (config.archivoListaNegra()))
        for (auto* e : xml->getChildWithTagNameIterator ("X"))
            listaNegra[e->getStringAttribute ("clave")] = e->getStringAttribute ("mod").getLargeIntValue();
}

void Escaner::guardarListaNegra() const
{
    juce::XmlElement raiz ("NEGRA");
    {
        std::lock_guard<std::mutex> g (m);
        for (auto& [clave, mod] : listaNegra)
        {
            auto* e = raiz.createNewChildElement ("X");
            e->setAttribute ("clave", clave);
            e->setAttribute ("mod", juce::String (mod));
        }
    }
    raiz.writeTo (config.archivoListaNegra());
}

//==============================================================================
bool Escaner::escanearUno (const juce::String& formato, const juce::String& archivo,
                           std::vector<juce::PluginDescription>& destino, juce::String& motivo)
{
    juce::ChildProcess hijo;
    if (! hijo.start (juce::StringArray { programa.getFullPathName(), "--escanear-uno", formato, archivo },
                      juce::ChildProcess::wantStdOut))
    {
        motivo = "no se pudo lanzar el proceso de escaneo";
        return false;
    }

    if (! hijo.waitForProcessToFinish (plazoMs))
    {
        hijo.kill();
        motivo = "tardó más de "_es + juce::String (plazoMs / 1000) + " s en responder";
        return false;
    }

    const auto salida = hijo.readAllProcessOutput();
    const auto codigo = hijo.getExitCode();
    const int inicio = salida.indexOf ("<RESULTADO");
    if (inicio < 0)
    {
        // Sin resultado: el proceso murió por una señal (JUCE da entonces código 0) o salió con error.
        motivo = "se cayó al cargarlo (el proceso terminó sin dar resultado, código "_es + juce::String ((int) codigo) + ")";
        return false;
    }

    if (auto xml = juce::XmlDocument::parse (salida.substring (inicio)))
        for (auto* p : xml->getChildWithTagNameIterator ("PLUGIN"))
        {
            juce::PluginDescription d;
            if (d.loadFromXml (*p))
                destino.push_back (d);
        }

    if (destino.empty())
    {
        juce::String fallo;
        if (auto xml = juce::XmlDocument::parse (salida.substring (inicio)))
            fallo = xml->getStringAttribute ("fallo");
        motivo = fallo.isNotEmpty() ? "no se pudo cargar: "_es + fallo : "no contiene ningún plugin que se pueda cargar"_es;
        return false;
    }
    return true;
}

void Escaner::run()
{
    registro.escribir ("Escaneo de plugins: empieza" + juce::String (forzando ? " (completo)" : ""));

    struct Pendiente
    {
        juce::String formato, archivo;
    };
    std::vector<Pendiente> pendientes;

    juce::StringArray extras = config.carpetasExtra();
    extras.addArray (carpetasTemporales);

    for (int i = 0; i < formatos.getNumFormats(); ++i)
    {
        auto* f = formatos.getFormat (i);
        auto rutas = f->getDefaultLocationsToSearch();
        for (auto& e : extras)
            rutas.addIfNotAlreadyThere (juce::File (e));
        for (auto& a : f->searchPathsForPlugins (rutas, true, false))
            pendientes.push_back ({ f->getName(), a });
    }

    // Primero los VST3 y al final los AudioUnit: son los que más se usan en el estudio y los AU de algunas casas se quedan
    // esperando a una licencia o a un permiso de macOS, así que no deben retrasar a los demás.
    const auto rango = [] (const juce::String& formato) { return formato == "VST3" ? 0 : (formato == "AudioUnit" ? 2 : 1); };
    std::stable_sort (pendientes.begin(), pendientes.end(), [&] (const Pendiente& x, const Pendiente& y) { return rango (x.formato) < rango (y.formato); });

    // Fabricante de un AudioUnit (el tercer campo de «aufx,T503,Ikmm»); vacío si no es un AU.
    const auto fabricanteAU = [] (const juce::File& f)
    {
        juce::StringArray campos;
        campos.addTokens (f.getFileName(), ",", "");
        return campos.size() == 3 ? campos[2] : juce::String();
    };
    std::map<juce::String, int> sinRespuestaSeguidos;       // por fabricante: AU seguidos que no respondieron a tiempo

    std::vector<PluginFallido> fallidos;
    std::map<juce::String, Entrada> nueva;
    std::vector<juce::PluginDescription> todos;

    for (size_t k = 0; k < pendientes.size(); ++k)
    {
        if (threadShouldExit())
            return;

        const auto& [formato, archivo] = pendientes[k];
        const auto clave = formato + "|" + archivo;
        const juce::File f (archivo);
        const int64_t mod = f.exists() ? f.getLastModificationTime().toMilliseconds() : 0;

        if (alProgreso)
            alProgreso (f.getFileName(), (double) k / (double) pendientes.size());

        bool reutilizar = false;
        {
            std::lock_guard<std::mutex> g (m);
            auto it = cache.find (clave);
            if (! forzando && it != cache.end() && it->second.modificado == mod && mod != 0)
            {
                nueva[clave] = it->second;
                todos.insert (todos.end(), it->second.plugins.begin(), it->second.plugins.end());
                reutilizar = true;
            }
            else if (! forzando && listaNegra.count (clave) != 0 && listaNegra[clave] == mod)
            {
                fallidos.push_back ({ archivo, "se salta: falló la última vez y el archivo no ha cambiado"_es });
                reutilizar = true;
            }
        }
        if (reutilizar)
            continue;

        const auto fabricante = fabricanteAU (f);
        if (fabricante.isNotEmpty() && sinRespuestaSeguidos[fabricante] >= 2)
        {
            // Dos seguidos de la misma casa sin responder: lo normal es que esperen a una licencia o a un permiso. Se dejan para
            // la próxima vez (no entran en la lista negra) en vez de perder 30 s con cada uno.
            fallidos.push_back ({ archivo, "se salta esta vez: los anteriores de su fabricante no respondieron"_es });
            continue;
        }

        Entrada entrada;
        entrada.modificado = mod;
        juce::String motivo;
        if (escanearUno (formato, archivo, entrada.plugins, motivo))
        {
            if (fabricante.isNotEmpty())
                sinRespuestaSeguidos[fabricante] = 0;
            todos.insert (todos.end(), entrada.plugins.begin(), entrada.plugins.end());
            nueva[clave] = std::move (entrada);
            std::lock_guard<std::mutex> g (m);
            listaNegra.erase (clave);
            // Lo que ya se ha encontrado se puede usar sin esperar al final del escaneo (con muchos plugins, tarda).
            for (auto& d : todos)
            {
                const bool ya = std::any_of (actuales.begin(), actuales.end(), [&] (const juce::PluginDescription& x) { return x.isDuplicateOf (d); });
                if (! ya)
                    actuales.push_back (d);
            }
        }
        else
        {
            registro.escribir ("Escaneo: " + f.getFileName() + ": " + motivo);
            fallidos.push_back ({ archivo, motivo });
            if (fabricante.isNotEmpty() && motivo.contains ("tardó más"))
                ++sinRespuestaSeguidos[fabricante];
            std::lock_guard<std::mutex> g (m);
            listaNegra[clave] = mod;
        }
    }

    {
        std::lock_guard<std::mutex> g (m);
        cache = std::move (nueva);
        actuales = todos;
    }
    guardarCache();
    guardarListaNegra();
    registro.escribir ("Escaneo de plugins: " + juce::String ((int) todos.size()) + " encontrados, " + juce::String ((int) fallidos.size()) + " con problemas");

    if (alProgreso)
        alProgreso ({}, 1.0);
    if (alTerminar)
        alTerminar ((int) todos.size(), fallidos);
}

//==============================================================================
int escanearArchivoYEscribir (const juce::String& nombreFormato, const juce::String& archivo)
{
    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();

    juce::AudioPluginFormat* formato = nullptr;
    for (int i = 0; i < fm.getNumFormats(); ++i)
        if (fm.getFormat (i)->getName() == nombreFormato)
            formato = fm.getFormat (i);
    if (formato == nullptr)
        return 3;

    juce::OwnedArray<juce::PluginDescription> descripciones;
    formato->findAllTypesForFile (descripciones, archivo);

   #if JUCE_MAC
    // Si JUCE no encuentra ningún plugin en un paquete, el motivo real casi nunca sale en su mensaje. Se intenta cargar el binario a pelo
    // y se cuenta qué dice macOS (arquitectura equivocada, firma, una biblioteca que falta…). Sirve para que el usuario sepa por qué.
    juce::String motivoReal;
    if (descripciones.isEmpty() && nombreFormato == "VST3")
    {
        juce::File binario;
        for (auto& f : juce::File (archivo).getChildFile ("Contents/MacOS").findChildFiles (juce::File::findFiles, false))
            { binario = f; break; }
        if (binario.existsAsFile())
        {
            if (void* h = ::dlopen (binario.getFullPathName().toRawUTF8(), RTLD_LAZY | RTLD_LOCAL))
            {
                motivoReal = "el binario se carga, pero no ofrece ningún plugin VST3 (¿pide una licencia o un host concreto?)"_es;
                ::dlclose (h);
            }
            else
            {
                const char* crudo = ::dlerror();
                const auto e = juce::String::fromUTF8 (crudo != nullptr ? crudo : "motivo desconocido");
                // El texto de macOS: «… incompatible architecture (have 'x86_64', need 'arm64e' or 'arm64')». Se saca lo que importa.
                if (e.contains ("incompatible architecture") && e.contains ("have 'x86_64'") && e.contains ("need 'arm64"))
                    motivoReal = "es solo para Intel y este Mac es Apple Silicon: necesita una versión nueva del plugin (o abrir SYNZ Bridge con Rosetta)"_es;
                else if (e.contains ("incompatible architecture") && e.contains ("have 'arm64") && e.contains ("need 'x86_64'"))
                    motivoReal = "es solo para Apple Silicon y SYNZ Bridge está abierto con Rosetta (como Intel)"_es;
                else
                    motivoReal = "macOS no lo carga: "_es + e.substring (0, 240);
            }
        }
        else
            motivoReal = "no tiene binario en Contents/MacOS"_es;
    }
   #endif

    // JUCE saca la descripción de un VST3 de su `moduleinfo.json` sin crear el plugin. Aquí SÍ se crea y se prepara,
    // en este proceso aparte: un plugin que se cae al cargarse mata a este hijo y no al puente, y lo que se devuelve
    // ya lleva los canales reales.
    juce::XmlElement raiz ("RESULTADO");
    juce::String fallo;
    for (auto* d : descripciones)
    {
        juce::String error;
        auto plugin = fm.createPluginInstance (*d, 44100.0, 512, error);
        if (plugin == nullptr)
        {
            fallo = error.isEmpty() ? "no se pudo crear el plugin"_es : error;
            continue;
        }
        plugin->prepareToPlay (44100.0, 512);
        juce::PluginDescription real;
        plugin->fillInPluginDescription (real);
        plugin->releaseResources();
        plugin.reset();
        // Se queda la descripción original (la que encuentra el formato por su nombre y su archivo) y solo se toma
        // de la real lo que el plugin sabe de sí mismo una vez creado.
        auto copia = *d;
        copia.numInputChannels = real.numInputChannels;
        copia.numOutputChannels = real.numOutputChannels;
        copia.isInstrument = real.isInstrument;
        raiz.addChildElement (copia.createXml().release());
    }
   #if JUCE_MAC
    if (raiz.getNumChildElements() == 0 && fallo.isEmpty() && motivoReal.isNotEmpty())
        fallo = motivoReal;
   #endif
    if (raiz.getNumChildElements() == 0 && fallo.isNotEmpty())
        raiz.setAttribute ("fallo", fallo);

    std::cout << raiz.toString().toStdString() << std::endl;
    return 0;
}
} // namespace synz
