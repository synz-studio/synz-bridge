// SPDX-License-Identifier: AGPL-3.0-only
#include "SensoresOsc.h"
#include "Registro.h"
#include "Texto.h"

namespace synz
{
namespace
{
    /** Un id o una salida del estudio: a-z, 0-9, - y _ (el registro del estudio no admite otra cosa). */
    juce::String limpia (const juce::String& t, int largo)
    {
        juce::String r;
        for (auto c : t.toLowerCase())
        {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') r << juce::String::charToString (c);
            else if (c == ' ' || c == '.' || c == '/') r << "-";
            if (r.length() >= largo) break;
        }
        while (r.startsWith ("-") || r.startsWith ("_")) r = r.substring (1);
        return r;
    }

    /** Un rótulo de 4 letras: «fader1» → «fad1», «indice» → «indi»; con varios argumentos, la letra del eje: «xy1» → «xy1x», «xy1y». */
    juce::String corto (const juce::String& base, int arg, int args)
    {
        if (args > 1)
            return base.substring (0, 3) + juce::String::charToString ("xyzwabcd"[juce::jlimit (0, 7, arg)]);
        if (base.length() <= 4)
            return base;
        int i = base.length();
        while (i > 0 && juce::CharacterFunctions::isDigit (base[i - 1])) --i;
        const auto cifras = base.substring (i).getLastCharacters (2);
        return base.substring (0, 4 - cifras.length()) + cifras;
    }

    bool numero (const juce::OSCArgument& a, float& v)
    {
        if (a.isFloat32()) { v = a.getFloat32(); return std::isfinite (v); }
        if (a.isInt32())   { v = (float) a.getInt32(); return true; }
        return false;
    }

    juce::var objeto (std::initializer_list<std::pair<const char*, juce::var>> campos)
    {
        auto* o = new juce::DynamicObject();
        for (auto& c : campos)
            o->setProperty (c.first, c.second);
        return juce::var (o);
    }
}

SensoresOsc::SensoresOsc (Registro& r) : log (r) {}

SensoresOsc::~SensoresOsc()
{
    detener();
}

int SensoresOsc::iniciar (int puerto, bool todaLaRed)
{
    detener();
    if (puerto < 0)
        return -1;
    socket = std::make_unique<juce::DatagramSocket> (false);
    if (! socket->bindToPort (puerto, todaLaRed ? juce::String() : juce::String ("127.0.0.1")))
    {
        log.escribir ("OSC: no se pudo abrir el puerto UDP "_es + juce::String (puerto) + " (¿lo usa otro programa?)"_es);
        socket.reset();
        return -1;
    }
    puertoAbierto = socket->getBoundPort();
    redAbierta = todaLaRed;
    receptor.addListener (this);
    receptor.connectToSocket (*socket);
    startTimer (8);
    log.escribir ("OSC para los sensores del estudio en "_es + (todaLaRed ? "toda la red" : "127.0.0.1") + ":" + juce::String (puertoAbierto) + " (UDP)");
    return puertoAbierto;
}

void SensoresOsc::detener()
{
    stopTimer();
    receptor.removeListener (this);
    receptor.disconnect();
    socket.reset();
    puertoAbierto = -1;
    std::vector<std::shared_ptr<WsConexion>> d;
    {
        std::lock_guard<std::mutex> g (m);
        d.swap (destinos);
    }
    for (auto& w : d)
        w->cerrar (1001);
}

void SensoresOsc::agregar (std::shared_ptr<WsConexion> ws)
{
    std::lock_guard<std::mutex> g (m);
    if ((int) destinos.size() >= maxConexiones)
    {
        ws->cerrar (1013);
        return;
    }
    destinos.push_back (ws);
    for (auto& s : lista)
        s.alta = true;           // la conexión nueva tiene que conocerlos: se mandan otra vez a todas (es inofensivo)
    for (auto& s : lista)
        for (auto& x : s.salidas)
            x.cambio = true;
    hayCambios = true;
}

void SensoresOsc::quitar (const std::shared_ptr<WsConexion>& ws)
{
    std::lock_guard<std::mutex> g (m);
    destinos.erase (std::remove (destinos.begin(), destinos.end(), ws), destinos.end());
}

int SensoresOsc::conexiones() const
{
    std::lock_guard<std::mutex> g (m);
    return (int) destinos.size();
}

int SensoresOsc::sensores() const
{
    std::lock_guard<std::mutex> g (m);
    return (int) lista.size();
}

SensoresOsc::Sensor* SensoresOsc::sensor (const juce::String& id, const juce::String& nombre)
{
    for (auto& s : lista)
        if (s.id == id)
            return &s;
    if (id.isEmpty() || (int) lista.size() >= maxSensores)
        return nullptr;
    lista.push_back ({ id, nombre, {}, true });
    return &lista.back();
}

SensoresOsc::Salida* SensoresOsc::salida (Sensor& s, const juce::String& id, const juce::String& clave, const juce::String& rotulo)
{
    const auto& k = clave.isNotEmpty() ? clave : id;
    for (auto& x : s.salidas)
        if (x.clave == k)
            return &x;
    if (id.isEmpty() || (int) s.salidas.size() >= maxSalidas)
        return nullptr;
    Salida nueva;
    nueva.id = id;
    nueva.clave = k;
    nueva.rotulo = rotulo.isNotEmpty() ? rotulo : corto (id, 0, 1);
    s.salidas.push_back (nueva);
    s.alta = true;
    return &s.salidas.back();
}

void SensoresOsc::oscMessageReceived (const juce::OSCMessage& msg)
{
    recibir (msg);
}

void SensoresOsc::oscBundleReceived (const juce::OSCBundle& b)
{
    for (auto& e : b)
    {
        if (e.isMessage()) recibir (e.getMessage());
        else if (e.isBundle()) oscBundleReceived (e.getBundle());
    }
}

void SensoresOsc::recibir (const juce::OSCMessage& msg)
{
    const auto dir = msg.getAddressPattern().toString();
    juce::StringArray partes;
    partes.addTokens (dir, "/", "");
    partes.removeEmptyStrings();

    std::lock_guard<std::mutex> g (m);
    const auto pon = [this] (Salida* x, float v) { if (x != nullptr) { x->valor = v; x->cambio = true; hayCambios = true; } };

    // /synz/rango/<id>/<salida> min max
    if (partes.size() == 4 && partes[0] == "synz" && partes[1] == "rango")
    {
        float a = 0, b = 1;
        if (msg.size() < 2 || ! numero (msg[0], a) || ! numero (msg[1], b) || a == b) return;
        const auto id = limpia (partes[2], 32);
        if (auto* s = sensor (id, id))
            if (auto* x = salida (*s, limpia (partes[3], 24)))
            {
                x->min = a; x->max = b; s->alta = true; hayCambios = true;
            }
        return;
    }
    // /synz/sensor/<id>[/<salida>] v…
    if (partes.size() >= 3 && partes[0] == "synz" && partes[1] == "sensor")
    {
        const auto id = limpia (partes[2], 32);
        auto* s = sensor (id, partes[2].substring (0, 48));
        if (s == nullptr) return;
        if (partes.size() >= 4)
        {
            float v = 0;
            if (msg.size() >= 1 && numero (msg[0], v))
                pon (salida (*s, limpia (partes[3], 24)), v);
        }
        else
            for (int i = 0; i < msg.size() && i < maxSalidas; ++i)
            {
                float v = 0;
                if (numero (msg[i], v))
                    pon (salida (*s, "s" + juce::String (i + 1)), v);
            }
        return;
    }
    // cualquier otra dirección: el sensor «osc», una salida por dirección y argumento (rótulo: la última parte de la dirección)
    if (partes.isEmpty()) return;
    auto* s = sensor ("osc", "OSC");
    if (s == nullptr) return;
    const auto base = limpia (partes[partes.size() - 1], 20);
    for (int i = 0; i < msg.size() && i < maxSalidas; ++i)
    {
        float v = 0;
        if (! numero (msg[i], v)) continue;
        const auto id = msg.size() > 1 ? base + "-" + juce::String (i + 1) : base;
        pon (salida (*s, id.isNotEmpty() ? id : "s" + juce::String (s->salidas.size() + 1), dir + "#" + juce::String (i), corto (base, i, msg.size())), v);
    }
}

juce::var SensoresOsc::altaDe (const Sensor& s)
{
    juce::Array<juce::var> sal;
    for (auto& x : s.salidas)
        sal.add (objeto ({ { "id", x.id }, { "l", x.rotulo }, { "min", x.min }, { "max", x.max },
                           { "h", objeto ({ { "es", "OSC: " + x.clave.upToLastOccurrenceOf ("#", false, false) }, { "en", "OSC: " + x.clave.upToLastOccurrenceOf ("#", false, false) } }) } }));
    return objeto ({ { "synz", "sensor" }, { "id", s.id }, { "nombre", objeto ({ { "es", s.nombre }, { "en", s.nombre } }) }, { "salidas", sal } });
}

void SensoresOsc::hiResTimerCallback()
{
    std::vector<std::shared_ptr<WsConexion>> d;
    juce::Array<juce::var> mensajes;
    {
        std::lock_guard<std::mutex> g (m);
        if (! hayCambios || destinos.empty())
            return;
        hayCambios = false;
        d = destinos;
        for (auto& s : lista)
        {
            if (s.salidas.empty())
                continue;
            if (s.alta)
            {
                mensajes.add (altaDe (s));
                s.alta = false;
            }
            auto* v = new juce::DynamicObject();
            bool alguno = false;
            for (auto& x : s.salidas)
                if (x.cambio)
                {
                    v->setProperty (x.id, x.valor);
                    x.cambio = false;
                    alguno = true;
                }
            if (alguno)
                mensajes.add (objeto ({ { "id", s.id }, { "v", juce::var (v) } }));
            else
                delete v;
        }
    }
    if (mensajes.isEmpty())
        return;
    const auto texto = juce::JSON::toString (juce::var (mensajes), true);
    for (auto& w : d)
        if (w->abierta())
            w->enviarTexto (texto);
}
} // namespace synz
