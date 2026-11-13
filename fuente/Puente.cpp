#include "Puente.h"
#include "Texto.h"
#include <random>

namespace synz
{
namespace
{
    juce::var objeto (std::initializer_list<std::pair<const char*, juce::var>> campos)
    {
        auto* o = new juce::DynamicObject();
        for (auto& c : campos)
            o->setProperty (c.first, c.second);
        return juce::var (o);
    }

    juce::var errorVar (const char* codigo, const juce::String& texto)
    {
        return objeto ({ { "ok", false }, { "codigo", codigo }, { "error", texto } });
    }

    juce::var okVar()
    {
        return objeto ({ { "ok", true } });
    }

    juce::String aJson (const juce::var& v)
    {
        return juce::JSON::toString (v, true);
    }

    juce::String ticketNuevo()
    {
        std::random_device rd;          // entropía del sistema operativo
        juce::String t;
        for (int i = 0; i < 4; ++i)
            t << juce::String::toHexString ((juce::int64) rd()).paddedLeft ('0', 8).getLastCharacters (8);
        return t;
    }

    juce::String sistema()
    {
       #if JUCE_MAC
        return "mac";
       #elif JUCE_WINDOWS
        return "windows";
       #else
        return "linux";
       #endif
    }

    /** Ejecuta `f` en el hilo de mensajes y espera el resultado. */
    template <typename F>
    juce::var enMensajes (F&& f)
    {
        if (auto r = juce::MessageManager::callSync (std::forward<F> (f)))
            return *r;
        return errorVar ("no_disponible", "el puente se está cerrando"_es);
    }

    juce::String valorDeConsulta (const juce::String& consulta, const juce::String& clave)
    {
        juce::StringArray partes;
        partes.addTokens (consulta, "&", "");
        for (auto& p : partes)
            if (p.upToFirstOccurrenceOf ("=", false, false) == clave)
                return p.fromFirstOccurrenceOf ("=", false, false);
        return {};
    }

    constexpr int maxControles = 8;
    constexpr int maxInstancias = 32;
    constexpr size_t maxTramaAudio = 16 + (size_t) maxMidiPorBloque * 8 + (size_t) maxCanales * maxBloque * 4;
}

//==============================================================================
Puente::Puente (Opciones o)
    : opciones (std::move (o)),
      config (opciones.carpetaDatos),
      log (config.archivoRegistro()),
      escaner (config, log, formatos, opciones.programa),
      osc (log),
      servidor (*this)
{
    escaner.plazoMs = opciones.plazoEscaneoMs;
    escaner.carpetasTemporales = opciones.carpetasTemporales;
}

Puente::~Puente()
{
    detener();
}

bool Puente::iniciar()
{
    formatos.addDefaultFormats();
    escaner.alProgreso = [this] (const juce::String& actual, double progreso)
    {
        difundir (objeto ({ { "evento", "escaneo" }, { "actual", actual }, { "progreso", progreso } }));
    };
    escaner.alTerminar = [this] (int total, const std::vector<PluginFallido>& fallidos)
    {
        juce::Array<juce::var> lista;
        for (auto& f : fallidos)
            lista.add (objeto ({ { "archivo", juce::File (f.archivo).getFileName() }, { "motivo", f.motivo } }));
        difundir (objeto ({ { "evento", "escaneoTerminado" }, { "total", total }, { "fallidos", lista } }));
        notificarCambio();
    };

    if (servidor.iniciar (opciones.puerto) < 0)
    {
        log.escribir ("No se pudo abrir el puerto " + juce::String (opciones.puerto) + ": ¿hay otro puente abierto?"_es);
        return false;
    }
    log.escribir ("Escuchando en 127.0.0.1:" + juce::String (servidor.puerto()));
    if (opciones.puertoOsc >= 0)
        osc.iniciar (opciones.puertoOsc, opciones.oscRed || config.oscRed());   // si el puerto está ocupado, el puente sigue sin OSC
    escaner.iniciar (false);
    startTimer (33);
    return true;
}

void Puente::detener()
{
    if (detenido)
        return;
    detenido = true;
    *vivo = false;
    stopTimer();
    escaner.alProgreso = nullptr;
    escaner.alTerminar = nullptr;

    // Las conexiones pueden estar esperando al hilo de mensajes (crear, abrir editor…): mientras se paran,
    // se sigue atendiendo ese hilo para que no se bloqueen unas a otras.
    osc.detener();       // cierra también las conexiones de /sensores, que si no esperarían a su cliente
    std::atomic<bool> parado { false };
    std::thread parador ([&] { servidor.parar(); parado = true; });
    while (! parado.load())
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    parador.join();

    std::vector<std::shared_ptr<Instancia>> restantes;
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        for (auto& [id, inst] : instancias)
            restantes.push_back (inst);
        instancias.clear();
        audios.clear();
    }
    for (auto& i : restantes)
        i->liberar();
}

Puente::Resumen Puente::resumen() const
{
    Resumen r;
    {
        std::lock_guard<std::mutex> g (mutexControl);
        r.conexiones = (int) controles.size();
    }
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        r.instancias = (int) instancias.size();
    }
    r.plugins = (int) escaner.lista().size();
    r.escaneando = escaner.escaneando();
    r.oscPuerto = osc.puerto();
    r.oscRed = osc.enRed();
    r.oscSensores = osc.sensores();
    r.oscConexiones = osc.conexiones();
    return r;
}

void Puente::notificarCambio()
{
    auto estoyVivo = vivo;
    juce::MessageManager::callAsync ([this, estoyVivo]
    {
        if (estoyVivo->load() && alCambiar)
            alCambiar();
    });
}

//==============================================================================
// Quién puede usar el puente
bool Puente::origenPermitido (const juce::String& origen)
{
    if (config.origenSiempre (origen) || opciones.origenesPermitidos.contains (origen))
        return true;

    auto decidido = [&] (bool& resultado)
    {
        std::lock_guard<std::mutex> g (mutexSesion);
        if (permitidosEstaVez.contains (origen)) { resultado = true; return true; }
        auto it = denegadosHasta.find (origen);
        if (it != denegadosHasta.end() && it->second > juce::Time::currentTimeMillis()) { resultado = false; return true; }
        return false;
    };

    bool res = false;
    if (decidido (res))
        return res;
    if (! opciones.preguntar)
        return false;

    std::lock_guard<std::mutex> una (mutexPregunta);     // una pregunta cada vez
    if (decidido (res))
        return res;

    struct Estado
    {
        std::atomic<int> resultado { 0 };   // 1 siempre, 2 esta vez, 0 denegar
        juce::WaitableEvent evento;
    };
    auto st = std::make_shared<Estado>();
    auto estoyVivo = vivo;
    log.escribir ("Pide permiso el origen " + origen);

    juce::MessageManager::callAsync ([st, origen, estoyVivo]
    {
        if (! estoyVivo->load())
        {
            st->evento.signal();
            return;
        }
        auto opc = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::QuestionIcon)
                       .withTitle ("SYNZ Bridge")
                       .withMessage ("La web  " + origen + "  quiere usar los plugins de este ordenador.\n\n¿La dejas conectarse?"_es)
                       .withButton ("Permitir siempre")
                       .withButton ("Solo esta vez")
                       .withButton ("Denegar");
        juce::AlertWindow::showAsync (opc, [st] (int r)
        {
            st->resultado = (r == 1) ? 1 : (r == 2 ? 2 : 0);
            st->evento.signal();
        });
    });

    st->evento.wait (30000);
    const int r = st->resultado.load();
    if (r == 1)
    {
        config.permitirSiempre (origen);
        log.escribir ("Origen aceptado para siempre: " + origen);
        return true;
    }
    std::lock_guard<std::mutex> g (mutexSesion);
    if (r == 2)
    {
        permitidosEstaVez.addIfNotAlreadyThere (origen);
        log.escribir ("Origen aceptado esta vez: " + origen);
        return true;
    }
    denegadosHasta[origen] = juce::Time::currentTimeMillis() + 60000;   // no se vuelve a preguntar durante un minuto
    log.escribir ("Origen denegado: " + origen);
    return false;
}

bool Puente::rutaValida (const juce::String& ruta)
{
    return ruta == "/control" || ruta == "/sensores" || ruta.startsWith ("/audio/");
}

bool Puente::prioridadAlta (const SolicitudWs& s)
{
    return s.ruta.startsWith ("/audio/");
}

juce::String Puente::textoRaiz()
{
    return "synz-puente " + juce::String (versionProtocolo);
}

void Puente::atender (std::shared_ptr<WsConexion> conexion, const SolicitudWs& solicitud)
{
    if (solicitud.ruta == "/control")
        atenderControl (std::move (conexion), solicitud);
    else if (solicitud.ruta == "/sensores")
        atenderSensores (std::move (conexion), solicitud);
    else
        atenderAudio (std::move (conexion), solicitud);
}

//==============================================================================
// Sensores (PROTOCOLO.md §7): el puente manda; lo que llegue del estudio (su saludo) se lee y se descarta
void Puente::atenderSensores (std::shared_ptr<WsConexion> ws, const SolicitudWs& solicitud)
{
    log.escribir ("Sensores OSC: conectado "_es + solicitud.origen);
    osc.agregar (ws);
    notificarCambio();
    std::vector<uint8_t> datos;
    bool texto = false;
    while (ws->abierta())
    {
        const auto r = ws->leer (datos, texto, 1000);
        if (r == WsConexion::Resultado::TiempoAgotado || r == WsConexion::Resultado::Mensaje)
            continue;
        break;
    }
    osc.quitar (ws);
    notificarCambio();
}

//==============================================================================
// Control
void Puente::difundir (const juce::var& evento)
{
    const auto texto = aJson (evento);
    std::vector<std::shared_ptr<WsConexion>> destinos;
    {
        std::lock_guard<std::mutex> g (mutexControl);
        for (auto& c : controles)
            destinos.push_back (c.ws);
    }
    for (auto& d : destinos)
        d->enviarTexto (texto);
}

void Puente::enviarA (int propietario, const juce::var& evento)
{
    std::shared_ptr<WsConexion> destino;
    {
        std::lock_guard<std::mutex> g (mutexControl);
        for (auto& c : controles)
            if (c.id == propietario)
                destino = c.ws;
    }
    if (destino != nullptr)
        destino->enviarTexto (aJson (evento));
}

void Puente::atenderControl (std::shared_ptr<WsConexion> ws, const SolicitudWs& solicitud)
{
    ConexionControl cc { siguienteControl.fetch_add (1), ws, solicitud.origen };
    {
        std::lock_guard<std::mutex> g (mutexControl);
        if ((int) controles.size() >= maxControles)
        {
            ws->cerrar (1013);      // «inténtalo más tarde»
            return;
        }
        controles.push_back (cc);
    }
    log.escribir ("Conexión de control "_es + juce::String (cc.id) + " desde " + cc.origen);
    notificarCambio();

    std::vector<uint8_t> datos;
    datos.reserve (4096);
    bool texto = false;
    while (ws->abierta())
    {
        const auto r = ws->leer (datos, texto, 1000);
        if (r == WsConexion::Resultado::TiempoAgotado)
            continue;
        if (r != WsConexion::Resultado::Mensaje)
            break;
        if (! texto)
        {
            ws->cerrar (1003);
            break;
        }

        const auto peticion = juce::JSON::parse (juce::String::fromUTF8 (reinterpret_cast<const char*> (datos.data()), (int) datos.size()));
        juce::var resp;
        if (! peticion.isObject())
            resp = errorVar ("peticion_invalida", "se esperaba un objeto JSON");
        else
        {
            try
            {
                resp = operacion (cc, peticion);
            }
            catch (...)
            {
                resp = errorVar ("fallo_plugin", "error inesperado al atender la petición"_es);
            }
        }
        if (auto* o = resp.getDynamicObject())
            o->setProperty ("id", peticion.getProperty ("id", juce::var()));
        if (! ws->enviarTexto (aJson (resp)))
            break;
    }

    {
        std::lock_guard<std::mutex> g (mutexControl);
        controles.erase (std::remove_if (controles.begin(), controles.end(), [&] (auto& c) { return c.id == cc.id; }), controles.end());
    }
    destruirInstanciasDe (cc.id);
    log.escribir ("Conexión de control "_es + juce::String (cc.id) + " cerrada");
    notificarCambio();
}

std::shared_ptr<Instancia> Puente::instanciaDe (const ConexionControl& c, const juce::var& p)
{
    const int id = (int) p.getProperty ("instancia", -1);
    std::lock_guard<std::mutex> g (mutexInstancias);
    auto it = instancias.find (id);
    if (it == instancias.end() || it->second->propietario != c.id)
        return nullptr;     // una conexión solo ve lo suyo
    return it->second;
}

juce::var Puente::operacion (ConexionControl& c, const juce::var& p)
{
    const auto op = p.getProperty ("op", juce::var()).toString();

    if (op == "hola")
    {
        juce::Array<juce::var> fs;
        for (int i = 0; i < formatos.getNumFormats(); ++i)
            fs.add (formatos.getFormat (i)->getName());
        auto r = okVar();
        auto* o = r.getDynamicObject();
        o->setProperty ("protocolo", versionProtocolo);
        o->setProperty ("puente", versionPuente);
        o->setProperty ("sistema", sistema());
        o->setProperty ("formatos", fs);
        o->setProperty ("plugins", (int) escaner.lista().size());
        o->setProperty ("escaneando", escaner.escaneando());
        {
            std::lock_guard<std::mutex> g (mutexInstancias);
            o->setProperty ("instancias", (int) instancias.size());
        }
        juce::Array<juce::var> caps;
        for (auto c : { "capturarEditor", "escalarEditor", "ventanaSynz", "sensoresOsc" })
            caps.add (c);
        o->setProperty ("capacidades", caps);
        o->setProperty ("osc", osc.puerto());       // el puerto UDP de los sensores (-1 si no está abierto)       // lo que sabe hacer este puente: el estudio avisa si le falta algo que usa
        return r;
    }

    if (op == "listar")
    {
        juce::Array<juce::var> lista;
        for (auto& d : escaner.lista())
            lista.add (objeto ({ { "id", d.createIdentifierString() },
                                 { "nombre", d.name },
                                 { "fabricante", d.manufacturerName },
                                 { "version", d.version },
                                 { "formato", d.pluginFormatName },
                                 { "categoria", d.category },
                                 { "instrumento", d.isInstrument },
                                 { "entradas", d.numInputChannels },
                                 { "salidas", d.numOutputChannels } }));
        auto r = okVar();
        r.getDynamicObject()->setProperty ("plugins", lista);
        return r;
    }

    if (op == "escanear")
    {
        const bool ya = escaner.escaneando();
        escaner.iniciar ((bool) p.getProperty ("forzar", false));
        auto r = okVar();
        r.getDynamicObject()->setProperty ("iniciado", ! ya);
        return r;
    }

    if (op == "crear")
        return opCrear (c, p);

    static const juce::StringArray opsDeInstancia { "destruir", "abrirEditor", "cerrarEditor", "capturarEditor", "escalarEditor", "leerEstado", "ponerEstado",
                                                    "leerParametros", "ponerParametro" };
    if (! opsDeInstancia.contains (op))
        return errorVar ("operacion_desconocida", "operación desconocida: "_es + op);

    // Todo lo que sigue es sobre una instancia propia.
    auto inst = instanciaDe (c, p);
    if (inst == nullptr)
        return errorVar ("instancia_desconocida", "esa instancia no existe o no es de esta conexión"_es);

    if (op == "destruir")
    {
        const int id = inst->id;
        return enMensajes ([this, id]() -> juce::var { destruirInstancia (id); return okVar(); });
    }

    if (op == "abrirEditor")
    {
        const float escala = juce::jlimit (0.0f, 3.0f, (float) (double) p.getProperty ("escala", 0.0));
        if (! inst->tieneEditor())
            return errorVar ("no_disponible", "este plugin no tiene ventana");
        const int id = inst->id;
        return enMensajes ([this, id, inst, escala]() -> juce::var
        {
            const bool abierta = inst->abrirEditor ([this, id, w = std::weak_ptr<Instancia> (inst), estoyVivo = vivo]
            {
                // El botón de cerrar se pulsa dentro de la propia ventana: se cierra un poco después, no desde dentro.
                juce::MessageManager::callAsync ([this, id, w, estoyVivo]
                {
                    if (! estoyVivo->load())
                        return;
                    if (auto i = w.lock())
                    {
                        i->cerrarEditor();
                        enviarA (i->propietario, objeto ({ { "evento", "editorCerrado" }, { "instancia", id } }));
                    }
                });
            }, escala);
            if (! abierta)
            {
                registro().escribir ("Un plugin no pudo crear su ventana (instancia "_es + juce::String (id) + ")"_es);
                return errorVar ("fallo_plugin", "el plugin no pudo crear su ventana");
            }
           #if JUCE_MAC
            registro().escribir ("Ventana de plugin (instancia "_es + juce::String (id) + "): "_es + estadoVentanaMac());
           #endif
            return okVar();
        });
    }

    if (op == "escalarEditor")
    {
        const float escala = juce::jlimit (0.25f, 3.0f, (float) (double) p.getProperty ("escala", 1.0));
        return enMensajes ([inst, escala]() -> juce::var
        {
            if (! inst->escalarEditor (escala))
                return errorVar ("no_disponible", "la ventana no está abierta");
            return okVar();
        });
    }

    if (op == "capturarEditor")
    {
        return enMensajes ([inst]() -> juce::var
        {
            juce::String jpeg;
            int ancho = 0, alto = 0;
            if (! inst->capturarEditor (jpeg, ancho, alto))
                return errorVar ("no_disponible", "no se pudo hacer la foto de la ventana (¿está abierta?)");
            auto r = okVar();
            r.getDynamicObject()->setProperty ("imagen", jpeg);
            r.getDynamicObject()->setProperty ("tipo", "image/jpeg");
            r.getDynamicObject()->setProperty ("ancho", ancho);
            r.getDynamicObject()->setProperty ("alto", alto);
            return r;
        });
    }

    if (op == "cerrarEditor")
    {
        return enMensajes ([inst]() -> juce::var { inst->cerrarEditor(); return okVar(); });
    }

    if (op == "leerEstado")
    {
        return enMensajes ([inst]() -> juce::var
        {
            auto r = okVar();
            r.getDynamicObject()->setProperty ("estado", inst->leerEstado());
            return r;
        });
    }

    if (op == "ponerEstado")
    {
        const auto estado = p.getProperty ("estado", juce::var()).toString();
        return enMensajes ([inst, estado]() -> juce::var
        {
            if (! inst->ponerEstado (estado))
                return errorVar ("peticion_invalida", "el estado no es base64 válido"_es);
            auto r = okVar();
            r.getDynamicObject()->setProperty ("parametros", inst->parametros());
            return r;
        });
    }

    if (op == "leerParametros")
    {
        return enMensajes ([inst]() -> juce::var
        {
            auto r = okVar();
            r.getDynamicObject()->setProperty ("valores", inst->valores());
            return r;
        });
    }

    if (op == "ponerParametro")
    {
        const int i = (int) p.getProperty ("i", -1);
        const float v = (float) (double) p.getProperty ("valor", 0.0);
        return enMensajes ([inst, i, v]() -> juce::var
        {
            if (! inst->ponerParametro (i, v))
                return errorVar ("peticion_invalida", "parámetro fuera de rango"_es);
            return okVar();
        });
    }

    return errorVar ("operacion_desconocida", "operación desconocida: "_es + op);
}

juce::var Puente::opCrear (ConexionControl& c, const juce::var& p)
{
    const auto idPlugin = p.getProperty ("plugin", juce::var()).toString();
    juce::PluginDescription descripcion;
    if (! escaner.buscar (idPlugin, descripcion))
        return errorVar ("plugin_desconocido", "ese plugin no está en la lista"_es);

    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        if ((int) instancias.size() >= maxInstancias)
            return errorVar ("limite", "demasiados plugins abiertos a la vez");
    }

    Instancia::Pedido pedido;
    pedido.frecuencia = (double) p.getProperty ("frecuencia", 48000.0);
    pedido.bloqueMax  = (int) p.getProperty ("bloqueMax", 512);
    pedido.entradas   = (int) p.getProperty ("entradas", -1);
    pedido.salidas    = (int) p.getProperty ("salidas", -1);

    const int origenId = c.id;
    const auto origen = c.origen;
    auto r = enMensajes ([&]() -> juce::var
    {
        int nuevoId;
        {
            std::lock_guard<std::mutex> g (mutexInstancias);
            nuevoId = siguienteInstancia++;
        }
        juce::String error;
        std::shared_ptr<Instancia> inst;
        try
        {
            inst = Instancia::crear (nuevoId, formatos, descripcion, pedido, error);
        }
        catch (...)
        {
            error = "el plugin lanzó una excepción al cargarse"_es;
        }
        if (inst == nullptr)
        {
            log.escribir ("No se pudo cargar " + descripcion.name + ": " + error);
            return errorVar ("fallo_plugin", error);
        }
        inst->ticket = ticketNuevo();
        inst->origen = origen;
        inst->propietario = origenId;
        {
            std::lock_guard<std::mutex> g (mutexInstancias);
            instancias[nuevoId] = inst;
        }
        log.escribir ("Plugin cargado: " + descripcion.name + " (instancia " + juce::String (nuevoId) + ")");
        auto resp = inst->describir();
        resp.getDynamicObject()->setProperty ("ok", true);
        return resp;
    });
    notificarCambio();
    return r;
}

void Puente::destruirInstancia (int id)
{
    JUCE_ASSERT_MESSAGE_THREAD
    std::shared_ptr<Instancia> inst;
    std::shared_ptr<WsConexion> audio;
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        auto it = instancias.find (id);
        if (it == instancias.end())
            return;
        inst = it->second;
        instancias.erase (it);
        auto a = audios.find (id);
        if (a != audios.end())
        {
            audio = a->second;
            audios.erase (a);
        }
    }
    if (audio != nullptr)
        audio->cortar();        // el hilo de audio de esta instancia termina
    inst->liberar();
    log.escribir ("Plugin descargado (instancia " + juce::String (id) + ")");
    notificarCambio();
}

void Puente::destruirInstanciasDe (int propietario)
{
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        for (auto& [id, inst] : instancias)
            if (inst->propietario == propietario)
                ids.push_back (id);
    }
    for (int id : ids)
    {
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            destruirInstancia (id);
        else if (! juce::MessageManager::callSync ([this, id] { destruirInstancia (id); }))
        {
            // El programa se está cerrando: `detener()` se ocupa de lo que quede.
        }
    }
}

//==============================================================================
// Audio
void Puente::atenderAudio (std::shared_ptr<WsConexion> ws, const SolicitudWs& solicitud)
{
    const int id = solicitud.ruta.fromFirstOccurrenceOf ("/audio/", false, false).getIntValue();
    std::shared_ptr<Instancia> inst;
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        auto it = instancias.find (id);
        if (it != instancias.end())
            inst = it->second;
    }
    // Hace falta el ticket que devolvió `crear` y el mismo origen que la creó.
    if (inst == nullptr || inst->ticket != valorDeConsulta (solicitud.consulta, "t") || inst->origen != solicitud.origen)
    {
        ws->cerrar (1008);
        return;
    }
    if (inst->audioConectado.exchange (true))
    {
        ws->cerrar (1008);          // ya tiene su canal de audio
        return;
    }
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        if (instancias.find (id) == instancias.end())
        {
            inst->audioConectado = false;
            ws->cerrar (1008);
            return;
        }
        audios[id] = ws;
    }

    std::vector<uint8_t> peticion, respuesta;
    peticion.reserve (maxTramaAudio);
    respuesta.reserve (16 + (size_t) maxCanales * maxBloque * 4);
    bool texto = false;

    while (ws->abierta())
    {
        const auto r = ws->leer (peticion, texto, 1000);
        if (r == WsConexion::Resultado::TiempoAgotado)
        {
            std::lock_guard<std::mutex> g (mutexInstancias);
            if (instancias.find (id) == instancias.end())
                break;
            continue;
        }
        if (r != WsConexion::Resultado::Mensaje)
            break;
        if (texto || peticion.size() > maxTramaAudio)
        {
            ws->cerrar (texto ? 1003 : 1009);
            break;
        }
        if (inst->procesar (peticion.data(), peticion.size(), respuesta) != Instancia::Fallo::Ninguno)
        {
            ws->cerrar (1002);
            break;
        }
        if (! ws->enviarBinario (respuesta.data(), respuesta.size()))
            break;
    }

    inst->audioConectado = false;
    std::lock_guard<std::mutex> g (mutexInstancias);
    auto a = audios.find (id);
    if (a != audios.end() && a->second == ws)
        audios.erase (a);
}

//==============================================================================
void Puente::timerCallback()
{
    std::vector<std::shared_ptr<Instancia>> copia;
    {
        std::lock_guard<std::mutex> g (mutexInstancias);
        for (auto& [id, inst] : instancias)
            copia.push_back (inst);
    }
    std::vector<std::pair<int, float>> cambios;
    for (auto& i : copia)
    {
        cambios.clear();
        if (i->recogerCambios (cambios))
            for (auto& [indice, valor] : cambios)
                enviarA (i->propietario, objeto ({ { "evento", "parametro" }, { "instancia", i->id }, { "i", indice }, { "valor", (double) valor } }));
        if (i->avisoCaidaPendiente())
        {
            log.escribir ("El plugin de la instancia " + juce::String (i->id) + " falló al procesar audio y se ha silenciado"_es);
            enviarA (i->propietario, objeto ({ { "evento", "instanciaCaida" }, { "instancia", i->id }, { "motivo", "excepción al procesar audio"_es } }));
        }
    }
}
} // namespace synz
