// El núcleo del puente: atiende las conexiones de control y de audio (protocolo en PROTOCOLO.md), lleva las
// instancias de plugin y decide qué orígenes web pueden usar los plugins del ordenador.
#pragma once
#include "Config.h"
#include "Escaner.h"
#include "Instancia.h"
#include "Registro.h"
#include "SensoresOsc.h"
#include "ws/WsServidor.h"
#include <atomic>
#include <map>
#include <memory>

namespace synz
{
constexpr int versionProtocolo = 1;
constexpr const char* versionPuente = "1.3.0";   // 1.1.0: escalarEditor, capturarEditor, ventana de SYNZ, `capacidades` e `instancias` en «hola»; 1.3.0: sensores por OSC (/sensores)
constexpr int puertoPorDefecto = 47821;
constexpr int puertoOscPorDefecto = 9000;   // OSC (UDP) para los sensores del estudio

class Puente : public WsOyente, private juce::Timer
{
public:
    struct Opciones
    {
        int puerto = puertoPorDefecto;
        juce::File carpetaDatos;
        juce::File programa;                     // este mismo ejecutable (para lanzar el escáner)
        juce::StringArray origenesPermitidos;    // sin preguntar (pruebas y desarrollo)
        juce::StringArray carpetasTemporales;    // carpetas de plugins extra, solo esta ejecución
        bool preguntar = true;                   // pedir permiso al usuario para los orígenes nuevos
        int plazoEscaneoMs = 30000;
        int puertoOsc = puertoOscPorDefecto;    // -1 = sin OSC; 0 = que lo elija el sistema (pruebas)
        bool oscRed = false;                     // OSC también desde otros aparatos de la red (TouchOSC en una tableta)
    };

    explicit Puente (Opciones);
    ~Puente() override;

    /** Hilo de mensajes. Devuelve false si no se pudo abrir el puerto. */
    bool iniciar();
    int puerto() const { return servidor.puerto(); }
    int puertoOsc() const { return osc.puerto(); }

    Registro& registro() { return log; }
    Config& ajustes() { return config; }

    struct Resumen
    {
        int conexiones = 0, instancias = 0, plugins = 0;
        bool escaneando = false;
        int oscPuerto = -1, oscSensores = 0, oscConexiones = 0;
        bool oscRed = false;
    };
    Resumen resumen() const;
    void reescanear (bool forzar) { escaner.iniciar (forzar); }

    /** Hilo de mensajes. Para el servidor y suelta las instancias (sigue atendiendo mensajes mientras espera). */
    void detener();

    std::function<void()> alCambiar;     // algo cambió y la ventana puede repintarse (se llama en el hilo de mensajes)

    // WsOyente
    bool origenPermitido (const juce::String& origen) override;
    bool rutaValida (const juce::String& ruta) override;
    void atender (std::shared_ptr<WsConexion> conexion, const SolicitudWs& solicitud) override;
    bool prioridadAlta (const SolicitudWs& solicitud) override;
    juce::String textoRaiz() override;

private:
    struct ConexionControl
    {
        int id;
        std::shared_ptr<WsConexion> ws;
        juce::String origen;
    };

    void atenderControl (std::shared_ptr<WsConexion>, const SolicitudWs&);
    void atenderAudio (std::shared_ptr<WsConexion>, const SolicitudWs&);
    void atenderSensores (std::shared_ptr<WsConexion>, const SolicitudWs&);

    juce::var operacion (ConexionControl& c, const juce::var& peticion);   // devuelve el objeto de respuesta sin el id
    juce::var opCrear (ConexionControl& c, const juce::var& p);
    std::shared_ptr<Instancia> instanciaDe (const ConexionControl& c, const juce::var& p);

    void destruirInstancia (int id);                    // hilo de mensajes
    void destruirInstanciasDe (int propietario);        // cualquier hilo
    void difundir (const juce::var& evento);
    void enviarA (int propietario, const juce::var& evento);
    void notificarCambio();
    void timerCallback() override;

    Opciones opciones;
    Config config;       // antes que el registro: el registro escribe en su carpeta
    Registro log;
    juce::AudioPluginFormatManager formatos;
    Escaner escaner;
    SensoresOsc osc;     // antes que el servidor: las conexiones de /sensores lo usan
    WsServidor servidor;

    mutable std::mutex mutexControl;
    std::vector<ConexionControl> controles;
    std::atomic<int> siguienteControl { 1 };
    std::shared_ptr<std::atomic<bool>> vivo = std::make_shared<std::atomic<bool>> (true);
    bool detenido = false;

    mutable std::mutex mutexInstancias;
    std::map<int, std::shared_ptr<Instancia>> instancias;
    std::map<int, std::shared_ptr<WsConexion>> audios;
    int siguienteInstancia = 1;

    // Permiso de orígenes
    std::mutex mutexPregunta;
    std::mutex mutexSesion;
    juce::StringArray permitidosEstaVez;
    std::map<juce::String, juce::int64> denegadosHasta;
};
} // namespace synz
