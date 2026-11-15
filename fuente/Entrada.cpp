#include "Entrada.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace synz
{
namespace
{
    template <typename T>
    T leer (const uint8_t* p)
    {
        T v;
        std::memcpy (&v, p, sizeof (T));
        return v;
    }

    template <typename T>
    void escribirNum (uint8_t* p, T v)
    {
        std::memcpy (p, &v, sizeof (T));
    }

    const juce::String tipoPrueba = "Prueba";
    constexpr int canalesPrueba = 32;
    constexpr double fsPrueba = 48000.0;
    constexpr int bloquePrueba = 256;
}

//==============================================================================
/* EL DISPOSITIVO DE PRUEBA (solo con --entrada-prueba PPM): 32 entradas a 48 kHz, cada una con un tono propio (200 Hz + 50 Hz por
   canal, a media escala), escritas en bloques de 256 como una tarjeta, con SU reloj: va PPM más rápido (o más lento, si es negativo)
   que el reloj del ordenador. Así se prueba sin tarjeta que los canales llegan en su sitio, sin cortes y con la deriva absorbida. */
class Entrada::Prueba : public juce::Thread
{
public:
    Prueba (Entrada& d, double ppmNuevo, juce::Array<int> indicesNuevos)
        : juce::Thread ("SYNZ entrada de prueba"), duena (d), ppm (ppmNuevo), indices (std::move (indicesNuevos))
    {
        bloque.assign ((size_t) indices.size(), std::vector<float> ((size_t) bloquePrueba));
        for (auto& b : bloque)
            punteros.push_back (b.data());
    }
    ~Prueba() override { stopThread (2000); }

    void run() override
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const double fs = fsPrueba * (1.0 + ppm * 1e-6);
        juce::int64 hechas = 0;
        while (! threadShouldExit())
        {
            wait (2);
            const auto debidas = (juce::int64) ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001 * fs);
            while (debidas - hechas >= bloquePrueba && ! threadShouldExit())
            {
                for (size_t c = 0; c < bloque.size(); ++c)
                {
                    const double f = 200.0 + 50.0 * indices[(int) c];
                    for (int i = 0; i < bloquePrueba; ++i)
                        bloque[c][(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * f * (double) (hechas + i) / fsPrueba);
                }
                duena.escribir (punteros.data(), (int) punteros.size(), bloquePrueba);
                hechas += bloquePrueba;
            }
        }
    }

private:
    Entrada& duena;
    double ppm;
    juce::Array<int> indices;
    std::vector<std::vector<float>> bloque;
    std::vector<const float*> punteros;
};

//==============================================================================
juce::var Entrada::listar (bool conPrueba)
{
    juce::Array<juce::var> lista;
    if (conPrueba)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("tipo", tipoPrueba);
        o->setProperty ("nombre", "Prueba");
        o->setProperty ("entradas", canalesPrueba);
        o->setProperty ("frecuencias", juce::Array<juce::var> { fsPrueba });
        lista.add (juce::var (o));
    }
    juce::AudioDeviceManager gestorTemporal;
    for (auto* tipo : gestorTemporal.getAvailableDeviceTypes())
    {
        tipo->scanForDevices();
        for (auto& nombreDisp : tipo->getDeviceNames (true))
        {
            std::unique_ptr<juce::AudioIODevice> d (tipo->createDevice ({}, nombreDisp));
            if (d == nullptr)
                continue;
            const int n = d->getInputChannelNames().size();
            if (n <= 0)
                continue;
            juce::Array<juce::var> fs;
            for (auto f : d->getAvailableSampleRates())
                fs.add (f);
            auto* o = new juce::DynamicObject();
            o->setProperty ("tipo", tipo->getTypeName());
            o->setProperty ("nombre", nombreDisp);
            o->setProperty ("entradas", n);
            o->setProperty ("frecuencias", fs);
            lista.add (juce::var (o));
        }
    }
    return lista;
}

Entrada::Entrada (int idNuevo) : id (idNuevo) {}

Entrada::~Entrada()
{
    cerrar();
}

std::shared_ptr<Entrada> Entrada::abrir (int id, const Pedido& pedido, bool conPrueba, juce::String& error)
{
    std::shared_ptr<Entrada> e (new Entrada (id));
    if (! e->arrancar (pedido, conPrueba, error))
        return nullptr;
    return e;
}

bool Entrada::arrancar (const Pedido& p, bool conPrueba, juce::String& error)
{
    fsWeb = p.frecuencia > 8000.0 && p.frecuencia < 400000.0 ? p.frecuencia : 48000.0;
    bloqueMaximo = juce::jlimit (1, 2048, p.bloqueMax);
    tipoNombre = p.tipo;
    nombre = p.dispositivo;
    clave = p.tipo + "|" + p.dispositivo;

    // La prueba: tipo «Prueba», solo con --entrada-prueba. Su deriva (ppm) sale de la variable de entorno SYNZ_PRUEBA_PPM.
    if (p.tipo == tipoPrueba)
    {
        if (! conPrueba)
        {
            error = "no hay dispositivo de prueba (falta --entrada-prueba)";
            return false;
        }
        juce::Array<int> indices;
        for (auto c : p.canales)
            if (c >= 0 && c < canalesPrueba && ! indices.contains (c))
                indices.add (c);
        if (indices.isEmpty())
            for (int c = 0; c < canalesPrueba; ++c)
                indices.add (c);
        indices.sort();
        canales = indices.size();
        for (auto c : indices)
            nombresCanales.add ("Prueba " + juce::String (c + 1));
        preparar (fsPrueba, bloquePrueba);
        prueba = std::make_unique<Prueba> (*this, juce::SystemStats::getEnvironmentVariable ("SYNZ_PRUEBA_PPM", "0").getDoubleValue(), indices);
        prueba->startThread (juce::Thread::Priority::high);
        return true;
    }

    gestor = std::make_unique<juce::AudioDeviceManager>();
    juce::AudioIODeviceType* tipo = nullptr;
    for (auto* t : gestor->getAvailableDeviceTypes())
        if (t->getTypeName() == p.tipo)
            tipo = t;
    if (tipo == nullptr)
    {
        error = "ese tipo de dispositivo no existe: " + p.tipo;
        return false;
    }
    gestor->setCurrentAudioDeviceType (p.tipo, false);
    tipo->scanForDevices();
    std::unique_ptr<juce::AudioIODevice> sonda (tipo->createDevice ({}, p.dispositivo));
    if (sonda == nullptr)
    {
        error = "no se encuentra el dispositivo: " + p.dispositivo;
        return false;
    }
    const auto todos = sonda->getInputChannelNames();
    juce::Array<int> indices;
    for (auto c : p.canales)
        if (c >= 0 && c < todos.size() && ! indices.contains (c))
            indices.add (c);
    if (indices.isEmpty())
        for (int c = 0; c < juce::jmin (todos.size(), maxCanalesEntrada); ++c)
            indices.add (c);
    indices.sort();
    if (indices.size() > maxCanalesEntrada)
        indices.removeRange (maxCanalesEntrada, indices.size());
    // La frecuencia: la del estudio si la tarjeta la admite (así casi no hay que remuestrear); si no, la suya.
    double fs = 0.0;
    for (auto f : sonda->getAvailableSampleRates())
        if (std::abs (f - fsWeb) < 1.0)
            fs = f;
    sonda.reset();

    juce::AudioDeviceManager::AudioDeviceSetup s;
    s.inputDeviceName = p.dispositivo;
    s.outputDeviceName = {};
    s.useDefaultInputChannels = false;
    s.useDefaultOutputChannels = false;
    s.inputChannels.clear();
    for (auto c : indices)
        s.inputChannels.setBit (c);
    s.outputChannels.clear();
    s.sampleRate = fs;
    s.bufferSize = 0;
    canales = indices.size();
    for (auto c : indices)
        nombresCanales.add (todos[c]);

    const auto fallo = gestor->setAudioDeviceSetup (s, true);
    if (fallo.isNotEmpty() || gestor->getCurrentAudioDevice() == nullptr)
    {
        error = fallo.isNotEmpty() ? fallo : juce::String ("no se pudo abrir el dispositivo");
        gestor.reset();
        return false;
    }
    gestor->addAudioCallback (this);     // llama a audioDeviceAboutToStart, que prepara el búfer
    return true;
}

void Entrada::preparar (double fsDispositivo, int bloqueDispositivo)
{
    std::lock_guard<std::mutex> g (proceso);
    preparada = false;
    fsTarjeta = fsDispositivo > 0.0 ? fsDispositivo : 48000.0;
    const int capacidad = (int) (fsTarjeta * 2.0);
    fifo = std::make_unique<juce::AbstractFifo> (capacidad);
    anillo.assign ((size_t) canales, std::vector<float> ((size_t) capacidad));
    const int maxTramo = (int) std::ceil (bloqueMaximo * (fsTarjeta / fsWeb) * 1.01) + 16;
    tramo.assign ((size_t) canales, std::vector<float> ((size_t) maxTramo));
    interp.clear();
    for (int c = 0; c < canales; ++c)
        interp.push_back (std::make_unique<juce::LagrangeInterpolator>());
    // Lo que se quiere tener siempre en el búfer: dos bloques de la tarjeta (llega a golpes de un bloque) más el bloque más
    // grande que pida el estudio (en muestras de la tarjeta) y un poco de holgura para las prisas del navegador.
    objetivo = 2 * juce::jmax (64, bloqueDispositivo) + (int) std::ceil (bloqueMaximo * fsTarjeta / fsWeb) + 256;
    cebado = false;
    llenoSuave = 0.0;
    integral = 0.0;
    preparada = true;
}

void Entrada::cerrar()
{
    if (prueba != nullptr)
    {
        prueba->stopThread (2000);
        prueba.reset();
    }
    if (gestor != nullptr)
    {
        gestor->removeAudioCallback (this);
        gestor->closeAudioDevice();
        gestor.reset();
    }
    std::lock_guard<std::mutex> g (proceso);
    preparada = false;
}

juce::var Entrada::describir() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("instancia", id);
    o->setProperty ("ticket", ticket);
    o->setProperty ("tipo", tipoNombre);
    o->setProperty ("nombre", nombre);
    o->setProperty ("canales", canales);
    juce::Array<juce::var> ns;
    for (auto& n : nombresCanales)
        ns.add (n);
    o->setProperty ("nombres", ns);
    o->setProperty ("frecuenciaTarjeta", fsTarjeta);
    o->setProperty ("latenciaTarjeta", latenciaTarjeta);
    o->setProperty ("objetivo", objetivo);
    return juce::var (o);
}

//==============================================================================
void Entrada::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    latenciaTarjeta = d->getInputLatencyInSamples();
    preparar (d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
}

void Entrada::audioDeviceIOCallbackWithContext (const float* const* entradas, int nEntradas, float* const* salidas, int nSalidas,
                                                int muestras, const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < nSalidas; ++c)
        if (salidas[c] != nullptr)
            juce::FloatVectorOperations::clear (salidas[c], muestras);
    escribir (entradas, nEntradas, muestras);
}

void Entrada::escribir (const float* const* ent, int nCanales, int muestras)
{
    if (! preparada.load() || fifo == nullptr)
        return;
    int s1, b1, s2, b2;
    fifo->prepareToWrite (muestras, s1, b1, s2, b2);
    if (b1 + b2 < muestras)
        nCortes.fetch_add (1);           // el estudio no lee (el búfer se llenó): lo que no cabe se pierde
    for (int c = 0; c < canales; ++c)
    {
        float* a = anillo[(size_t) c].data();
        const float* src = c < nCanales ? ent[c] : nullptr;
        if (src != nullptr)
        {
            if (b1 > 0) std::memcpy (a + s1, src, (size_t) b1 * sizeof (float));
            if (b2 > 0) std::memcpy (a + s2, src + b1, (size_t) b2 * sizeof (float));
        }
        else
        {
            if (b1 > 0) std::memset (a + s1, 0, (size_t) b1 * sizeof (float));
            if (b2 > 0) std::memset (a + s2, 0, (size_t) b2 * sizeof (float));
        }
    }
    fifo->finishedWrite (b1 + b2);
}

//==============================================================================
bool Entrada::procesar (const uint8_t* pet, size_t n, std::vector<uint8_t>& resp)
{
    if (n < 16)
        return false;
    const auto secuencia   = leer<uint32_t> (pet + 0);
    const int canalesPet   = leer<uint16_t> (pet + 4);
    const int frames       = leer<uint16_t> (pet + 6);
    const int nMidi        = leer<uint16_t> (pet + 8);
    if (canalesPet > 8 || frames < 1 || frames > bloqueMaximo || nMidi > 256)
        return false;
    if (n != 16 + (size_t) nMidi * 8 + (size_t) canalesPet * (size_t) frames * 4)
        return false;

    const size_t bytesSalida = (size_t) canales * (size_t) frames * 4;
    resp.resize (16 + bytesSalida);
    uint8_t* cab = resp.data();
    float* salida = reinterpret_cast<float*> (cab + 16);
    uint16_t banderas = 0;
    const auto t0 = juce::Time::getHighResolutionTicks();

    {
        std::lock_guard<std::mutex> g (proceso);
        bool silencio = true;
        if (preparada.load() && fifo != nullptr)
        {
            int disp = fifo->getNumReady();
            auto reinicia = [this] { for (auto& i : interp) i->reset(); };
            if (! cebado && disp >= objetivo)
            {
                cebado = true;
                llenoSuave = disp;
                integral = 0.0;
                reinicia();
            }
            if (cebado)
            {
                // El estudio se paró un rato (pestaña en segundo plano): lo que sobra se tira para no quedarse atrasado.
                if (disp > objetivo + (int) (fsTarjeta * 0.2))
                {
                    fifo->finishedRead (disp - objetivo);
                    disp = objetivo;
                    llenoSuave = disp;
                    reinicia();
                    nCortes.fetch_add (1);
                }
                // La corrección: proporcional a lo que sobra o falta (en segundos) más una parte lenta que la deja en cero.
                llenoSuave += 0.02 * ((double) disp - llenoSuave);
                const double err = (llenoSuave - objetivo) / fsTarjeta;
                const double dt = frames / fsWeb;
                // Con 0,5 se asienta en ~2 s (300 ppm de deriva dejan 0,6 ms de más) y la parte lenta lo lleva a cero en ~10 s.
                // La corrección máxima, 2000 ppm, es un cambio de afinación de 3,5 centésimas: no se oye.
                integral = juce::jlimit (-0.001, 0.001, integral + err * dt * 0.1);
                const double corr = juce::jlimit (-0.002, 0.002, err * 0.5 + integral);
                ppmActual = corr * 1e6;
                const double razon = fsTarjeta / fsWeb * (1.0 + corr);
                const int necesita = (int) std::ceil (frames * razon) + 4;
                if (disp < necesita || necesita > (int) tramo[0].size())
                {
                    cebado = false;                 // se vació: se espera a llenarlo otra vez
                    nCortes.fetch_add (1);
                }
                else
                {
                    int s1, b1, s2, b2;
                    fifo->prepareToRead (necesita, s1, b1, s2, b2);
                    int usadas = 0;
                    for (int c = 0; c < canales; ++c)
                    {
                        float* t = tramo[(size_t) c].data();
                        const float* a = anillo[(size_t) c].data();
                        if (b1 > 0) std::memcpy (t, a + s1, (size_t) b1 * sizeof (float));
                        if (b2 > 0) std::memcpy (t + b1, a + s2, (size_t) b2 * sizeof (float));
                        usadas = interp[(size_t) c]->process (razon, t, salida + (size_t) c * (size_t) frames, frames);
                    }
                    fifo->finishedRead (juce::jmin (usadas, b1 + b2));
                    silencio = false;
                }
            }
        }
        if (silencio)
        {
            std::memset (salida, 0, bytesSalida);
            banderas |= 4;                          // bit 2: no había audio de la tarjeta (silencio)
        }
    }

    const double usado = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const double disponible = frames / fsWeb;
    escribirNum<uint32_t> (cab + 0, secuencia);
    escribirNum<uint16_t> (cab + 4, (uint16_t) canales);
    escribirNum<uint16_t> (cab + 6, (uint16_t) frames);
    // «latencia»: lo que tarda en llegar al estudio lo que entra por la tarjeta (el búfer y la tarjeta), en muestras del estudio.
    escribirNum<uint32_t> (cab + 8, (uint32_t) std::lround ((llenoSuave + latenciaTarjeta) * fsWeb / fsTarjeta));
    escribirNum<uint16_t> (cab + 12, (uint16_t) juce::jlimit (0.0, 65535.0, usado / disponible * 10000.0));
    escribirNum<uint16_t> (cab + 14, banderas);
    return true;
}
} // namespace synz
