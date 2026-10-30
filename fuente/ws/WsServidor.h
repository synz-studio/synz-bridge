// Servidor HTTP/WebSocket mínimo, solo en el bucle local. Hace el handshake, comprueba `Host` y `Origin` y entrega
// cada conexión ya establecida a un `WsOyente`, en un hilo propio.
#pragma once
#include "WsConexion.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace synz
{
struct SolicitudWs
{
    juce::String ruta;       // sin la consulta: "/audio/3"
    juce::String consulta;   // lo que va tras «?», sin el «?»: "t=abc"
    juce::String origen;
};

class WsOyente
{
public:
    virtual ~WsOyente() = default;

    /** ¿Se acepta este origen? Se llama en el hilo de la conexión y PUEDE BLOQUEAR (por ejemplo, preguntando al
        usuario). El servidor ya comprobó que el origen no está vacío. */
    virtual bool origenPermitido (const juce::String& origen) = 0;

    /** ¿Existe esta ruta? Si no, se contesta 404 antes del handshake. */
    virtual bool rutaValida (const juce::String& ruta) = 0;

    /** Se llama con el handshake hecho, en el hilo de la conexión. Cuando vuelve, la conexión se cierra. */
    virtual void atender (std::shared_ptr<WsConexion> conexion, const SolicitudWs& solicitud) = 0;

    /** ¿Esta conexión lleva audio y su hilo debe tener la máxima prioridad? Se pregunta justo tras el handshake. */
    virtual bool prioridadAlta (const SolicitudWs&) { return false; }

    /** Texto de `GET /` (sin Upgrade). */
    virtual juce::String textoRaiz() = 0;
};

class WsServidor
{
public:
    struct Limites
    {
        int    conexionesMaximas = 64;
        size_t cargaMaxima       = 4u * 1024u * 1024u;
        int    cabecerasMaximas  = 8 * 1024;
        int    plazoHandshakeMs  = 5000;
    };

    WsServidor (WsOyente& oyente, Limites limites);
    WsServidor (WsOyente& o) : WsServidor (o, Limites {}) {}
    ~WsServidor();

    /** Empieza a escuchar en 127.0.0.1. Con `puerto` = 0 el sistema elige uno; devuelve el puerto real o -1. */
    int iniciar (int puerto);
    void parar();
    int puerto() const noexcept { return puertoReal; }

private:
    class HiloAceptar;
    class HiloConexion;

    void atenderSocket (std::unique_ptr<juce::StreamingSocket> socket, HiloConexion& hilo);
    bool handshake (juce::StreamingSocket& socket, SolicitudWs& solicitud, bool& conectado);
    void limpiarTerminados();

    WsOyente& oyente;
    const Limites limites;
    std::unique_ptr<juce::StreamingSocket> escucha;
    std::unique_ptr<HiloAceptar> aceptar;
    int puertoReal = -1;

    std::mutex vivasMutex;
    std::vector<std::weak_ptr<WsConexion>> vivas;   // para cortarlas todas al parar

    std::mutex mutexHilos;
    std::vector<std::unique_ptr<HiloConexion>> hilos;
    std::atomic<int> activas { 0 };
    std::atomic<bool> parando { false };

    friend class HiloConexion;
};

/** Ignora SIGPIPE (escribir en un socket cerrado no debe matar el programa). Llamar una vez al arrancar. */
void ignorarSigpipe();
} // namespace synz
