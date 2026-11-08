#include "WsConexion.h"

#if JUCE_WINDOWS
 #include <winsock2.h>
#else
 #include <sys/socket.h>
#endif

#include <cstring>

namespace synz
{
namespace
{
    // Tiempo máximo que se espera a que llegue el resto de una trama ya empezada. Un cliente que se queda a medias
    // no puede tener un hilo del puente atado para siempre.
    constexpr int plazoTramaMs = 10000;
}

WsConexion::WsConexion (std::unique_ptr<juce::StreamingSocket> s, size_t maximo)
    : socket (std::move (s)), cargaMaxima (maximo)
{
}

WsConexion::~WsConexion()
{
    cortar();
}

void WsConexion::cortar()
{
    viva = false;
    // Solo un hilo cierra el socket: `StreamingSocket::close()` no aguanta dos llamadas a la vez (libera la misma memoria dos veces
    // y macOS aborta el programa). Pasaba al descargar un plugin a la vez que la conexión de audio se cortaba sola.
    if (cortada.exchange (true))
        return;
    if (socket != nullptr)
        socket->close();
}

bool WsConexion::leerExacto (void* destino, size_t n)
{
    auto* p = static_cast<uint8_t*> (destino);
    size_t leidos = 0;
    while (leidos < n)
    {
        if (! viva.load() || socket == nullptr || ! socket->isConnected())
            return false;
        if (socket->waitUntilReady (true, plazoTramaMs) <= 0)
            return false;   // plazo vencido o error
        const int r = socket->read (p + leidos, (int) (n - leidos), false);
        if (r <= 0)
            return false;   // 0 = el otro lado cerró
        leidos += (size_t) r;
    }
    return true;
}

bool WsConexion::enviarTrama (uint8_t opcode, const void* a, size_t na, const void* b, size_t nb)
{
    std::lock_guard<std::mutex> g (escritura);
    if (! viva.load() || socket == nullptr)
        return false;

    const size_t n = na + nb;
    uint8_t cab[10];
    size_t nc = 0;
    cab[nc++] = (uint8_t) (0x80 | (opcode & 0x0f));       // fin + opcode
    if (n < 126)
        cab[nc++] = (uint8_t) n;
    else if (n <= 0xffff)
    {
        cab[nc++] = 126;
        cab[nc++] = (uint8_t) (n >> 8);
        cab[nc++] = (uint8_t) n;
    }
    else
    {
        cab[nc++] = 127;
        for (int i = 7; i >= 0; --i)
            cab[nc++] = (uint8_t) ((uint64_t) n >> (8 * i));
    }

    // Todo en un solo envío: tres escrituras pequeñas seguidas se notan en la latencia.
    static thread_local std::vector<uint8_t> salida;
    salida.resize (nc + n);
    std::memcpy (salida.data(), cab, nc);
    if (na > 0) std::memcpy (salida.data() + nc, a, na);
    if (nb > 0) std::memcpy (salida.data() + nc + na, b, nb);

    size_t enviados = 0;
    while (enviados < salida.size())
    {
        const int r = socket->write (salida.data() + enviados, (int) (salida.size() - enviados));
        if (r <= 0)
        {
            viva = false;
            return false;
        }
        enviados += (size_t) r;
    }
    return true;
}

bool WsConexion::enviarTexto (const juce::String& texto)
{
    const auto utf8 = texto.toRawUTF8();
    return enviarTrama (0x1, utf8, std::strlen (utf8), nullptr, 0);
}

bool WsConexion::enviarBinario (const void* datos, size_t n)
{
    return enviarTrama (0x2, datos, n, nullptr, 0);
}

bool WsConexion::enviarBinario (const void* a, size_t na, const void* b, size_t nb)
{
    return enviarTrama (0x2, a, na, b, nb);
}

void WsConexion::cierreProtocolo (uint16_t codigo)
{
    cerrar (codigo);
}

void WsConexion::cerrar (uint16_t codigo)
{
    {
        std::lock_guard<std::mutex> g (escritura);
        if (cierreEnviado || socket == nullptr)
            return;
        cierreEnviado = true;
    }

    const uint8_t carga[2] = { (uint8_t) (codigo >> 8), (uint8_t) codigo };
    enviarTrama (0x8, carga, 2, nullptr, 0);

    // Se cierra solo el lado de escritura y se deja un momento para que el cliente reciba el «close»: si se
    // cortara de golpe con datos suyos sin leer, el sistema mandaría un reset y el «close» se perdería.
    if (socket != nullptr && socket->getRawSocketHandle() >= 0)
    {
       #if JUCE_WINDOWS
        ::shutdown ((SOCKET) socket->getRawSocketHandle(), SD_SEND);
       #else
        ::shutdown (socket->getRawSocketHandle(), SHUT_WR);
       #endif
        uint8_t basura[256];
        const auto limite = juce::Time::getMillisecondCounter() + 300;
        while (juce::Time::getMillisecondCounter() < limite)
        {
            if (socket->waitUntilReady (true, 50) <= 0)
                continue;
            if (socket->read (basura, (int) sizeof (basura), false) <= 0)
                break;
        }
    }
    cortar();
}

WsConexion::Resultado WsConexion::leer (std::vector<uint8_t>& datos, bool& esTexto, int tiempoMs)
{
    datos.clear();
    bool enFragmento = false;
    esTexto = false;

    for (;;)
    {
        // Espera al primer byte del mensaje. Se hace por tramos para que `cortar()` desde otro hilo despierte esta
        // espera aunque no haya plazo.
        if (! enFragmento)
        {
            const auto limite = juce::Time::getMillisecondCounter() + (uint32_t) juce::jmax (0, tiempoMs);
            for (;;)
            {
                if (! viva.load() || socket == nullptr)
                    return Resultado::Cerrada;
                int espera = 250;
                if (tiempoMs >= 0)
                {
                    const auto ahora = juce::Time::getMillisecondCounter();
                    if (ahora >= limite)
                        return Resultado::TiempoAgotado;
                    espera = (int) juce::jmin<uint32_t> (250u, limite - ahora);
                }
                const int r = socket->waitUntilReady (true, espera);
                if (r < 0)
                {
                    viva = false;
                    return Resultado::Cerrada;
                }
                if (r > 0)
                    break;
            }
        }

        uint8_t cab[2];
        if (! leerExacto (cab, 2))
        {
            viva = false;
            return Resultado::Cerrada;
        }

        const bool fin     = (cab[0] & 0x80) != 0;
        const bool rsv     = (cab[0] & 0x70) != 0;
        const uint8_t op   = cab[0] & 0x0f;
        const bool mascara = (cab[1] & 0x80) != 0;
        uint64_t largo     = cab[1] & 0x7f;

        if (rsv || ! mascara)           // sin extensiones negociadas; el cliente siempre enmascara
        {
            cierreProtocolo (1002);
            return Resultado::Error;
        }

        if (largo == 126)
        {
            uint8_t e[2];
            if (! leerExacto (e, 2)) { viva = false; return Resultado::Cerrada; }
            largo = (uint64_t) e[0] << 8 | e[1];
        }
        else if (largo == 127)
        {
            uint8_t e[8];
            if (! leerExacto (e, 8)) { viva = false; return Resultado::Cerrada; }
            largo = 0;
            for (int i = 0; i < 8; ++i)
                largo = (largo << 8) | e[i];
            if (largo >> 63)            // el bit alto tiene que ser 0
            {
                cierreProtocolo (1002);
                return Resultado::Error;
            }
        }

        if (op >= 8 && (! fin || largo > 125))   // los controles no se fragmentan ni pasan de 125 bytes
        {
            cierreProtocolo (1002);
            return Resultado::Error;
        }

        // El tamaño se comprueba ANTES de reservar memoria.
        if (largo > cargaMaxima || (op < 8 && datos.size() + largo > cargaMaxima))
        {
            cierreProtocolo (1009);
            return Resultado::Error;
        }

        uint8_t clave[4];
        if (! leerExacto (clave, 4)) { viva = false; return Resultado::Cerrada; }

        trama.resize ((size_t) largo);
        if (largo > 0 && ! leerExacto (trama.data(), (size_t) largo))
        {
            viva = false;
            return Resultado::Cerrada;
        }
        for (size_t i = 0; i < trama.size(); ++i)
            trama[i] ^= clave[i & 3];

        switch (op)
        {
            case 0x0:   // continuación
            case 0x1:   // texto
            case 0x2:   // binario
            {
                if (op == 0x0 && ! enFragmento)         { cierreProtocolo (1002); return Resultado::Error; }
                if (op != 0x0 && enFragmento)           { cierreProtocolo (1002); return Resultado::Error; }
                if (op != 0x0)
                {
                    esTexto = (op == 0x1);
                    enFragmento = true;
                }
                datos.insert (datos.end(), trama.begin(), trama.end());
                if (fin)
                {
                    if (esTexto && ! juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (datos.data()), (int) datos.size()))
                    {
                        cierreProtocolo (1007);
                        return Resultado::Error;
                    }
                    return Resultado::Mensaje;
                }
                break;
            }
            case 0x8:   // close
            {
                uint16_t codigo = 1000;
                if (trama.size() >= 2)
                    codigo = (uint16_t) (trama[0] << 8 | trama[1]);
                if (trama.size() == 1)
                {
                    cierreProtocolo (1002);
                    return Resultado::Error;
                }
                cerrar ((codigo >= 1000 && codigo < 5000) ? codigo : (uint16_t) 1002);
                return Resultado::Cerrada;
            }
            case 0x9:   // ping
                enviarTrama (0xA, trama.data(), trama.size(), nullptr, 0);
                break;
            case 0xA:   // pong
                break;
            default:
                cierreProtocolo (1002);
                return Resultado::Error;
        }
    }
}
} // namespace synz
