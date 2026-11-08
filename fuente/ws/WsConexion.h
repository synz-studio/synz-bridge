// Una conexión WebSocket ya establecida (handshake hecho): lee y escribe mensajes completos.
// Reglas de RFC 6455 que se comprueban: tramas del cliente siempre enmascaradas, sin bits RSV, controles con
// fin y ≤ 125 bytes, fragmentos en orden, texto en UTF-8 válido y tamaño máximo por mensaje.
#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace synz
{
class WsConexion
{
public:
    WsConexion (std::unique_ptr<juce::StreamingSocket> socket, size_t cargaMaxima);
    ~WsConexion();

    enum class Resultado
    {
        Mensaje,        // hay un mensaje completo en `datos`
        TiempoAgotado,  // no llegó nada en el plazo (solo si se pidió plazo)
        Cerrada,        // cierre ordenado (el cliente mandó «close» o se cortó el socket)
        Error           // el cliente rompió el protocolo; ya se cerró con el código que toca
    };

    /** Lee un mensaje completo (reensambla fragmentos, contesta a ping y a close).
        `tiempoMs` < 0 espera sin límite. Reutiliza la capacidad de `datos`. */
    Resultado leer (std::vector<uint8_t>& datos, bool& esTexto, int tiempoMs = -1);

    bool enviarTexto (const juce::String& texto);
    bool enviarBinario (const void* datos, size_t n);
    /** Un mensaje binario hecho de dos trozos contiguos (cabecera + audio) sin copiarlos juntos. */
    bool enviarBinario (const void* a, size_t na, const void* b, size_t nb);

    /** Manda «close» con código y cierra. */
    void cerrar (uint16_t codigo = 1000);
    /** Corta el socket ya, sin cortesías: desbloquea un `leer` que esté esperando en otro hilo. */
    void cortar();

    bool abierta() const noexcept { return viva.load(); }

private:
    bool leerExacto (void* destino, size_t n);
    bool enviarTrama (uint8_t opcode, const void* a, size_t na, const void* b, size_t nb);
    void cierreProtocolo (uint16_t codigo);

    std::unique_ptr<juce::StreamingSocket> socket;
    const size_t cargaMaxima;
    std::atomic<bool> viva { true };
    std::atomic<bool> cortada { false };
    std::mutex escritura;
    bool cierreEnviado = false;
    std::vector<uint8_t> trama;     // carga de la trama en curso
};
} // namespace synz
