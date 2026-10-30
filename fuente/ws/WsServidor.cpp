#include "WsServidor.h"
#include "Texto.h"
#include "Sha1.h"

#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
#else
 #include <netinet/in.h>
 #include <netinet/tcp.h>
 #include <signal.h>
 #include <sys/socket.h>
#endif

namespace synz
{
void ignorarSigpipe()
{
   #if ! JUCE_WINDOWS
    ::signal (SIGPIPE, SIG_IGN);
   #endif
}

namespace
{
    constexpr const char* guidWs = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    void sinRetardo (juce::StreamingSocket& s)
    {
        const int uno = 1;
       #if JUCE_WINDOWS
        const auto h = (SOCKET) s.getRawSocketHandle();
       #else
        const int h = s.getRawSocketHandle();
       #endif
        ::setsockopt (h, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*> (&uno), sizeof (uno));
    }

    bool escribirTodo (juce::StreamingSocket& s, const juce::String& texto)
    {
        const auto* p = texto.toRawUTF8();
        const size_t n = std::strlen (p);
        size_t enviados = 0;
        while (enviados < n)
        {
            const int r = s.write (p + enviados, (int) (n - enviados));
            if (r <= 0)
                return false;
            enviados += (size_t) r;
        }
        return true;
    }

    void responderHttp (juce::StreamingSocket& s, int codigo, const juce::String& frase, const juce::String& cuerpo,
                        const juce::String& extra = {})
    {
        const auto n = (int) std::strlen (cuerpo.toRawUTF8());
        juce::String r;
        r << "HTTP/1.1 " << codigo << " " << frase << "\r\n"
          << "Content-Type: text/plain; charset=utf-8\r\n"
          << "Content-Length: " << n << "\r\n"
          << "Connection: close\r\n"
          << extra
          << "\r\n" << cuerpo;
        escribirTodo (s, r);
    }

    bool tieneToken (const juce::String& valor, const juce::String& token)
    {
        juce::StringArray partes;
        partes.addTokens (valor, ",", "");
        for (auto& p : partes)
            if (p.trim().equalsIgnoreCase (token))
                return true;
        return false;
    }
}

//==============================================================================
class WsServidor::HiloConexion : public juce::Thread
{
public:
    HiloConexion (WsServidor& s, std::unique_ptr<juce::StreamingSocket> sock)
        : juce::Thread ("ws-conexion"), servidor (s), socket (std::move (sock)) {}

    ~HiloConexion() override { stopThread (4000); }

    void run() override
    {
        try
        {
            servidor.atenderSocket (std::move (socket), *this);
        }
        catch (...)
        {
            // Una excepción del manejador no puede tumbar el programa.
        }
        servidor.activas.fetch_sub (1);
        terminado = true;
    }

    std::atomic<bool> terminado { false };

    void subirPrioridad() { setPriority (Priority::highest); }

private:
    WsServidor& servidor;
    std::unique_ptr<juce::StreamingSocket> socket;
};

class WsServidor::HiloAceptar : public juce::Thread
{
public:
    HiloAceptar (WsServidor& s) : juce::Thread ("ws-aceptar"), servidor (s) {}
    ~HiloAceptar() override { stopThread (2000); }

    void run() override
    {
        while (! threadShouldExit() && ! servidor.parando.load())
        {
            std::unique_ptr<juce::StreamingSocket> nuevo (servidor.escucha->waitForNextConnection());
            if (nuevo == nullptr)
            {
                if (servidor.parando.load() || threadShouldExit())
                    break;
                juce::Thread::sleep (20);
                continue;
            }

            servidor.limpiarTerminados();
            if (servidor.activas.load() >= servidor.limites.conexionesMaximas)
            {
                nuevo->close();     // demasiadas: se rechaza sin más
                continue;
            }
            sinRetardo (*nuevo);
            servidor.activas.fetch_add (1);

            auto hilo = std::make_unique<HiloConexion> (servidor, std::move (nuevo));
            auto* crudo = hilo.get();
            {
                std::lock_guard<std::mutex> g (servidor.mutexHilos);
                servidor.hilos.push_back (std::move (hilo));
            }
            crudo->startThread();
        }
    }

private:
    WsServidor& servidor;
};

//==============================================================================
WsServidor::WsServidor (WsOyente& o, Limites l) : oyente (o), limites (l) {}

WsServidor::~WsServidor()
{
    parar();
}

int WsServidor::iniciar (int puerto)
{
    parar();
    parando = false;
    escucha = std::make_unique<juce::StreamingSocket>();
    if (! escucha->createListener (puerto, "127.0.0.1"))
    {
        escucha.reset();
        return -1;
    }
    puertoReal = escucha->getBoundPort();
    aceptar = std::make_unique<HiloAceptar> (*this);
    aceptar->startThread();
    return puertoReal;
}

void WsServidor::parar()
{
    parando = true;
    if (escucha != nullptr)
        escucha->close();
    if (aceptar != nullptr)
    {
        aceptar->stopThread (2000);
        aceptar.reset();
    }
    escucha.reset();

    {
        std::lock_guard<std::mutex> g (mutexHilos);
        for (auto& h : hilos)
            h->signalThreadShouldExit();
    }
    {
        std::lock_guard<std::mutex> g (vivasMutex);
        for (auto& w : vivas)
            if (auto c = w.lock())
                c->cortar();
        vivas.clear();
    }
    std::vector<std::unique_ptr<HiloConexion>> aParar;
    {
        std::lock_guard<std::mutex> g (mutexHilos);
        aParar.swap (hilos);
    }
    aParar.clear();   // el destructor de cada hilo espera a que termine
    puertoReal = -1;
}

void WsServidor::limpiarTerminados()
{
    std::vector<std::unique_ptr<HiloConexion>> terminados;
    {
        std::lock_guard<std::mutex> g (mutexHilos);
        for (auto it = hilos.begin(); it != hilos.end();)
        {
            if ((*it)->terminado.load())
            {
                terminados.push_back (std::move (*it));
                it = hilos.erase (it);
            }
            else
                ++it;
        }
    }
    terminados.clear();
}

bool WsServidor::handshake (juce::StreamingSocket& socket, SolicitudWs& solicitud, bool& conectado)
{
    conectado = true;

    // Cabeceras: byte a byte, con tope de tamaño y de tiempo. Solo es el handshake.
    std::string cab;
    const auto limite = juce::Time::getMillisecondCounter() + (uint32_t) limites.plazoHandshakeMs;
    for (;;)
    {
        if (parando.load())
            return false;
        if (juce::Time::getMillisecondCounter() > limite)
        {
            responderHttp (socket, 408, "Request Timeout", "tiempo agotado");
            return false;
        }
        const int listo = socket.waitUntilReady (true, 100);
        if (listo < 0)
            return false;
        if (listo == 0)
            continue;
        char c;
        if (socket.read (&c, 1, false) != 1)
            return false;
        cab.push_back (c);
        if ((int) cab.size() > limites.cabecerasMaximas)
        {
            responderHttp (socket, 431, "Request Header Fields Too Large", "cabeceras demasiado grandes");
            return false;
        }
        const auto n = cab.size();
        if (n >= 4 && cab.compare (n - 4, 4, "\r\n\r\n") == 0)
            break;
    }

    juce::StringArray lineas;
    lineas.addLines (juce::String::fromUTF8 (cab.data(), (int) cab.size()));
    if (lineas.isEmpty())
    {
        responderHttp (socket, 400, "Bad Request", "petición vacía"_es);
        return false;
    }

    juce::StringArray rl;
    rl.addTokens (lineas[0], " ", "");
    rl.removeEmptyStrings();
    if (rl.size() != 3 || rl[2] != "HTTP/1.1" || ! rl[1].startsWithChar ('/'))
    {
        responderHttp (socket, 400, "Bad Request", "petición mal formada"_es);
        return false;
    }
    const auto metodo = rl[0];
    const auto destino = rl[1];

    std::map<juce::String, juce::String> h;
    for (int i = 1; i < lineas.size(); ++i)
    {
        if (lineas[i].isEmpty())
            continue;
        const int dp = lineas[i].indexOfChar (':');
        if (dp <= 0)
        {
            responderHttp (socket, 400, "Bad Request", "cabecera mal formada");
            return false;
        }
        const auto nombre = lineas[i].substring (0, dp).trim().toLowerCase();
        const auto valor = lineas[i].substring (dp + 1).trim();
        if ((nombre == "host" || nombre == "origin" || nombre == "sec-websocket-key") && h.count (nombre) != 0)
        {
            responderHttp (socket, 400, "Bad Request", "cabecera repetida");   // ambigüedad: no se acepta
            return false;
        }
        h[nombre] = valor;
    }

    // Host: solo el propio puente por su dirección de bucle local. Es lo que frena el «DNS rebinding».
    const auto p = juce::String (puertoReal);
    const auto host = h.count ("host") ? h["host"].toLowerCase() : juce::String();
    if (host != "127.0.0.1:" + p && host != "localhost:" + p)
    {
        responderHttp (socket, 403, "Forbidden", "host no permitido");
        return false;
    }

    const bool quiereWs = h.count ("upgrade") != 0 && h["upgrade"].equalsIgnoreCase ("websocket");
    const int q = destino.indexOfChar ('?');
    solicitud.ruta = q >= 0 ? destino.substring (0, q) : destino;
    solicitud.consulta = q >= 0 ? destino.substring (q + 1) : juce::String();

    if (! quiereWs)
    {
        if (metodo == "GET" && solicitud.ruta == "/" )
            responderHttp (socket, 200, "OK", oyente.textoRaiz());
        else
            responderHttp (socket, 404, "Not Found", "no existe");
        return false;
    }

    if (metodo != "GET" || ! h.count ("connection") || ! tieneToken (h["connection"], "upgrade"))
    {
        responderHttp (socket, 400, "Bad Request", "upgrade no válido"_es);
        return false;
    }
    if (! h.count ("sec-websocket-version") || h["sec-websocket-version"] != "13")
    {
        responderHttp (socket, 426, "Upgrade Required", "versión 13"_es, "Sec-WebSocket-Version: 13\r\n");
        return false;
    }

    juce::MemoryOutputStream claveBytes;
    const auto clave = h.count ("sec-websocket-key") ? h["sec-websocket-key"] : juce::String();
    if (clave.length() != 24 || ! juce::Base64::convertFromBase64 (claveBytes, clave) || claveBytes.getDataSize() != 16)
    {
        responderHttp (socket, 400, "Bad Request", "clave websocket no válida"_es);
        return false;
    }

    if (! oyente.rutaValida (solicitud.ruta))
    {
        responderHttp (socket, 404, "Not Found", "no existe");
        return false;
    }

    solicitud.origen = h.count ("origin") ? h["origin"] : juce::String();
    if (solicitud.origen.isEmpty() || ! oyente.origenPermitido (solicitud.origen))
    {
        responderHttp (socket, 403, "Forbidden", "origen no permitido");
        return false;
    }

    const auto unido = (clave + guidWs).toStdString();
    const auto hash = sha1 (unido.data(), unido.size());
    juce::String r;
    r << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << juce::Base64::toBase64 (hash.data(), hash.size()) << "\r\n\r\n";
    return escribirTodo (socket, r);
}

void WsServidor::atenderSocket (std::unique_ptr<juce::StreamingSocket> socket, HiloConexion& hilo)
{
    SolicitudWs solicitud;
    bool conectado = true;
    if (! handshake (*socket, solicitud, conectado))
    {
        // Cierre suave: si se corta de golpe con datos del cliente sin leer (cabeceras enormes), Windows manda un reset y
        // la respuesta de error (403, 431…) se pierde antes de llegar. Se cierra la escritura y se deja un momento para leer.
        if (socket->getRawSocketHandle() >= 0)
        {
           #if JUCE_WINDOWS
            ::shutdown ((SOCKET) socket->getRawSocketHandle(), SD_SEND);
           #else
            ::shutdown (socket->getRawSocketHandle(), SHUT_WR);
           #endif
            uint8_t basura[1024];
            const auto limite = juce::Time::getMillisecondCounter() + 300;
            while (juce::Time::getMillisecondCounter() < limite)
            {
                if (socket->waitUntilReady (true, 50) <= 0)
                    continue;
                if (socket->read (basura, (int) sizeof (basura), false) <= 0)
                    break;
            }
        }
        socket->close();
        return;
    }

    if (oyente.prioridadAlta (solicitud))
        hilo.subirPrioridad();

    auto conexion = std::make_shared<WsConexion> (std::move (socket), limites.cargaMaxima);
    {
        std::lock_guard<std::mutex> g (vivasMutex);
        vivas.erase (std::remove_if (vivas.begin(), vivas.end(), [] (auto& w) { return w.expired(); }), vivas.end());
        vivas.push_back (conexion);
    }
    if (parando.load())
    {
        conexion->cortar();
        return;
    }
    oyente.atender (conexion, solicitud);
    conexion->cerrar (1000);
}
} // namespace synz
