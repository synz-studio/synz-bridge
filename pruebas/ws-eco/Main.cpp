// Servidor eco para probar el servidor WebSocket solo, sin plugins ni interfaz.
//   ws-eco --puerto 0 --origen http://localhost:5555 [--carga-max 1000] [--max-conexiones 4]
// Escribe «LISTO <puerto>» en la salida cuando está escuchando y se para al cerrarse la entrada estándar o con SIGTERM.
#include "WsServidor.h"
#include <csignal>
#include <iostream>

namespace
{
std::atomic<bool> terminar { false };

class Eco : public synz::WsOyente
{
public:
    juce::StringArray permitidos;

    bool origenPermitido (const juce::String& o) override { return permitidos.contains (o); }
    bool rutaValida (const juce::String& r) override { return r == "/eco" || r == "/mudo"; }
    juce::String textoRaiz() override { return "synz-puente-eco 1"; }

    void atender (std::shared_ptr<synz::WsConexion> c, const synz::SolicitudWs& s) override
    {
        std::vector<uint8_t> datos;
        bool texto = false;
        for (;;)
        {
            const auto r = c->leer (datos, texto, 200);
            if (r == synz::WsConexion::Resultado::TiempoAgotado)
            {
                if (terminar.load()) return;
                continue;
            }
            if (r != synz::WsConexion::Resultado::Mensaje)
                return;
            if (s.ruta == "/mudo")
                continue;       // lee y no contesta
            if (texto)
            {
                const auto t = juce::String::fromUTF8 (reinterpret_cast<const char*> (datos.data()), (int) datos.size());
                if (t == "cerrar") { c->cerrar (1000); return; }
                if (! c->enviarTexto (t)) return;
            }
            else if (! c->enviarBinario (datos.data(), datos.size()))
                return;
        }
    }
};
} // namespace

int main (int argc, char** argv)
{
    synz::ignorarSigpipe();
    std::signal (SIGTERM, [] (int) { terminar = true; });
    std::signal (SIGINT,  [] (int) { terminar = true; });

    Eco eco;
    int puerto = 0;
    synz::WsServidor::Limites limites;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const auto sig = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--puerto") puerto = sig().getIntValue();
        else if (a == "--origen") eco.permitidos.add (sig());
        else if (a == "--carga-max") limites.cargaMaxima = (size_t) sig().getIntValue();
        else if (a == "--max-conexiones") limites.conexionesMaximas = sig().getIntValue();
    }

    synz::WsServidor servidor (eco, limites);
    const int real = servidor.iniciar (puerto);
    if (real < 0)
    {
        std::cerr << "no se pudo abrir el puerto\n";
        return 2;
    }
    std::cout << "LISTO " << real << std::endl;

    // Se queda hasta que se cierre la entrada estándar o llegue una señal.
    std::thread lector ([] { char c; while (std::cin.get (c)) {} terminar = true; });
    lector.detach();
    while (! terminar.load())
        juce::Thread::sleep (50);

    servidor.parar();
    return 0;
}
