// Un plugin cargado y en marcha. Se crea y se destruye en el hilo de mensajes; `procesar` lo llama el hilo del
// canal de audio de esa instancia, y no reserva memoria.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#if ! JUCE_LITTLE_ENDIAN
 #error "El protocolo del puente es little-endian y este código copia los números tal cual."
#endif

namespace synz
{
constexpr int maxCanales = 8;
constexpr int maxBloque  = 2048;
constexpr int maxMidiPorBloque = 256;

class Instancia : private juce::AudioProcessorParameter::Listener
{
public:
    struct Pedido
    {
        double frecuencia = 48000.0;
        int bloqueMax = 512;
        int entradas = -1;     // -1: lo habitual (los efectos 2, los instrumentos 0)
        int salidas = -1;      // -1: 2
    };

    /** Hilo de mensajes. Devuelve null y rellena `error` si el plugin no carga. */
    static std::shared_ptr<Instancia> crear (int id, juce::AudioPluginFormatManager& formatos,
                                             const juce::PluginDescription& descripcion, const Pedido& pedido,
                                             juce::String& error);
    ~Instancia() override;

    /** Hilo de mensajes. Cierra la ventana y suelta el plugin. Hay que llamarlo antes de que desaparezca la última
        referencia, y con el canal de audio ya cortado. */
    void liberar();

    const int id;
    juce::String ticket;                    // se pide en la conexión de audio
    juce::String origen;                    // origen web que la creó
    int propietario = 0;                    // id de la conexión de control que la creó
    std::atomic<bool> audioConectado { false };

    juce::var describir() const;            // lo que lleva la respuesta de `crear`
    bool esInstrumento() const noexcept { return instrumento; }

    //==============================================================================
    // Hilo de audio de la instancia
    enum class Fallo { Ninguno, PeticionMalFormada };
    /** Procesa un bloque. `respuesta` se reutiliza (su capacidad se conserva). */
    Fallo procesar (const uint8_t* peticion, size_t n, std::vector<uint8_t>& respuesta);

    //==============================================================================
    // Hilo de mensajes
    bool abrirEditor (std::function<void()> alCerrar, float escala = 0.0f);   // false si el plugin no pudo crear su ventana; `escala` 0 = la de siempre
    /** Zoom de la ventana abierta (1 = normal). Lo respetan los plugins que admiten escala (la mayoría de los modernos); la ventana sigue su tamaño. */
    bool escalarEditor (float escala);
    void cerrarEditor();
    /** Foto de la ventana del plugin (JPEG en base64, de 480 de ancho como mucho). Hay que haberla abierto antes. Solo en el hilo de mensajes. */
    bool capturarEditor (juce::String& jpegBase64, int& ancho, int& alto);
    bool tieneEditor() const noexcept { return editor; }

    juce::String leerEstado();
    bool ponerEstado (const juce::String& base64);
    juce::var valores() const;
    juce::var parametros() const;
    bool ponerParametro (int indice, float valor);

    /** Cambios que hizo el plugin por su cuenta (su ventana) desde la última vez. */
    bool recogerCambios (std::vector<std::pair<int, float>>& destino);
    bool caida() const noexcept { return caido.load(); }
    bool avisoCaidaPendiente() { return caido.load() && ! avisadaCaida.exchange (true); }

private:
    explicit Instancia (int idNuevo);
    void parameterValueChanged (int indice, float valor) override;
    void parameterGestureChanged (int, bool) override {}

    std::mutex proceso;                         // `procesar` lo toma entero; `liberar` espera a que acabe el bloque
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::String nombre;
    bool instrumento = false;
    bool editor = false;
    int entradas = 0, salidas = 2, canalesBuffer = 2;
    int bloqueMaximo = 512;
    double frecuencia = 48000.0;

    // Buffers preparados al crear
    std::vector<float> muestras;
    std::vector<float*> punteros;
    juce::MidiBuffer midi;
    struct EventoMidi { uint16_t desplazamiento; uint8_t estado, d1, d2, longitud; };
    EventoMidi eventos[maxMidiPorBloque];

    // Parámetros que se enseñan al estudio: solo los automatizables (el envoltorio VST3 de JUCE añade más de 2000 «MIDI CC»
    // que no lo son). El índice del protocolo es la posición en esta lista, no la del plugin.
    std::vector<int> visibles;                  // posición en el protocolo → índice real en el plugin
    std::vector<int> posicionVisible;           // índice real → posición en el protocolo (-1 si no se enseña)

    // Los listeners marcan (cualquier hilo) y el temporizador del hilo de mensajes recoge.
    std::vector<std::atomic<float>> pendiente;
    std::vector<std::atomic<bool>> sucio;
    std::vector<std::atomic<float>> puesto;     // lo último que puso el estudio, para no devolvérselo como si fuera del usuario
    std::atomic<bool> hayCambios { false };

    std::atomic<bool> caido { false };
    std::atomic<bool> avisadaCaida { false };

    class VentanaEditor;
    std::unique_ptr<VentanaEditor> ventana;
};
} // namespace synz
