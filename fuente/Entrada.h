// LAS ENTRADAS DE LA TARJETA DE SONIDO (13 nov 2026, para SYNZ Repertoire: «el maestro recibe cada puesto por cable»).
//
// Un navegador solo deja abrir dos canales de entrada de una tarjeta (Chrome lo tiene fijado en 2). El puente sí puede: abre la
// tarjeta con todas sus entradas (hasta 32) y se las da al estudio por el mismo canal de audio que los plugins (/audio/<id>),
// cuando el estudio pide cada bloque.
//
// Dos relojes: la tarjeta graba con el suyo (su cristal) y el estudio pide con el del navegador. Van casi igual, pero no del
// todo (unas decenas de ppm), y en una obra de 15 minutos eso son decenas de ms. Lo que llega de la tarjeta se guarda en un
// búfer por canal; al servir cada bloque se REMUESTREA (Lagrange) con una razón que se corrige sola según lo lleno que está el
// búfer, para que se quede siempre igual de lleno: sin cortes y con el retardo estable.
//
// El hilo de la tarjeta escribe; el hilo del canal de audio (el de la conexión /audio) lee. Una sola escritura y una sola
// lectura a la vez (AbstractFifo). Se abre y se cierra en el hilo de mensajes.
#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace synz
{
constexpr int maxCanalesEntrada = 32;

class Entrada : private juce::AudioIODeviceCallback
{
public:
    struct Pedido
    {
        juce::String tipo;            // tipo de dispositivo de JUCE («Windows Audio», «CoreAudio», «ALSA»…; «Prueba» con --entrada-prueba)
        juce::String dispositivo;     // nombre del dispositivo de entrada
        juce::Array<int> canales;     // índices de entrada de la tarjeta (0 = la 1); vacío = todas, hasta 32
        double frecuencia = 48000.0;  // la del estudio (AudioContext.sampleRate)
        int bloqueMax = 512;
    };

    /** Lo que hay: los dispositivos de entrada de cada tipo, con cuántas entradas tienen. Hilo de mensajes. */
    static juce::var listar (bool conPrueba);

    /** Hilo de mensajes. Devuelve null y rellena `error` si no se puede abrir. */
    static std::shared_ptr<Entrada> abrir (int id, const Pedido& pedido, bool conPrueba, juce::String& error);
    ~Entrada() override;

    /** Hilo de mensajes. Para la tarjeta. Hay que llamarlo con el canal de audio ya cortado. */
    void cerrar();

    const int id;
    juce::String ticket;
    juce::String origen;
    int propietario = 0;
    std::atomic<bool> audioConectado { false };
    juce::String clave;               // tipo + dispositivo: el mismo no se abre dos veces

    juce::var describir() const;

    /** Hilo del canal de audio. Una petición del protocolo (sin canales de entrada) → una respuesta con los canales de la tarjeta. */
    bool procesar (const uint8_t* peticion, size_t n, std::vector<uint8_t>& respuesta);

    // Para la ventana del puente y las pruebas.
    int cortes() const noexcept { return nCortes.load(); }
    double correccionPpm() const noexcept { return ppmActual.load(); }

private:
    explicit Entrada (int idNuevo);
    bool arrancar (const Pedido& pedido, bool conPrueba, juce::String& error);
    void preparar (double frecuenciaDispositivo, int bloqueDispositivo);
    void escribir (const float* const* canalesEntrada, int nCanales, int muestras);   // hilo de la tarjeta

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* entradas, int nEntradas, float* const* salidas, int nSalidas,
                                           int muestras, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* dispositivo) override;
    void audioDeviceStopped() override {}

    class Prueba;                                     // el dispositivo de prueba (sin tarjeta)

    std::mutex proceso;                               // `procesar` lo toma entero; `cerrar` espera a que acabe el bloque
    std::unique_ptr<juce::AudioDeviceManager> gestor;
    std::unique_ptr<Prueba> prueba;
    juce::String nombre, tipoNombre;
    juce::StringArray nombresCanales;
    int canales = 0;
    int bloqueMaximo = 512;
    double fsWeb = 48000.0, fsTarjeta = 48000.0;
    int latenciaTarjeta = 0;                          // muestras de la tarjeta (lo que dice el controlador)
    std::atomic<bool> preparada { false };

    // El búfer: un anillo por canal, con un solo índice (AbstractFifo) para todos.
    std::unique_ptr<juce::AbstractFifo> fifo;
    std::vector<std::vector<float>> anillo;
    std::vector<std::vector<float>> tramo;            // copia seguida de lo que se va a leer (el anillo puede dar la vuelta)
    std::vector<std::unique_ptr<juce::LagrangeInterpolator>> interp;

    // El control de la deriva (solo el hilo del canal de audio).
    bool cebado = false;
    int objetivo = 0;                                 // muestras de la tarjeta que se quieren en el búfer
    double llenoSuave = 0.0, integral = 0.0;
    std::atomic<int> nCortes { 0 };
    std::atomic<double> ppmActual { 0.0 };
};
} // namespace synz
