// SPDX-License-Identifier: AGPL-3.0-only
// SENSORES POR OSC (13 nov 2026, fase 3 de los sensores del estudio; ver docs/guidelines/sensores.md y PROTOCOLO.md §7).
// El puente escucha OSC por UDP (Max, Pure Data, SuperCollider, TouchOSC, Python…) y lo pasa al estudio por la ruta WebSocket
// /sensores con el mismo JSON que un programa del laboratorio (fase 2): un alta por sensor y después sus valores.
//
//   /synz/sensor/<id>/<salida> v          un valor de una salida de un sensor
//   /synz/sensor/<id> v1 v2 …             varias salidas a la vez, por orden (s1, s2…)
//   /synz/rango/<id>/<salida> min max     el rango de esa salida (el estudio lo pasa a 0..1); sin él, se espera 0..1
//   cualquier otra dirección, /1/fader1 v  va al sensor «osc»: una salida por dirección (y por argumento), por orden de llegada
//
// Hasta 8 sensores y 8 salidas por sensor; lo que sobra se ignora. Solo números (int32 y float32). Lo que llega se junta y se
// manda como mucho cada 8 ms, en un solo mensaje (una lista JSON). Por defecto solo escucha en 127.0.0.1; con `--osc-red`
// (o "oscRed": true en config.json), en todas las interfaces, para que TouchOSC mande desde una tableta de la misma red.
#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_osc/juce_osc.h>
#include "ws/WsConexion.h"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace synz
{
class Registro;

class SensoresOsc : private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>,
                    private juce::HighResolutionTimer
{
public:
    explicit SensoresOsc (Registro& registro);
    ~SensoresOsc() override;

    /** Abre el puerto UDP (0 = que lo elija el sistema). Devuelve el puerto abierto o -1. */
    int iniciar (int puerto, bool todaLaRed);
    void detener();
    int puerto() const noexcept { return puertoAbierto; }
    bool enRed() const noexcept { return redAbierta; }

    /** Una conexión del estudio a /sensores: recibe las altas de lo que ya se conoce y, desde ahí, todo lo nuevo. */
    void agregar (std::shared_ptr<WsConexion> ws);
    void quitar (const std::shared_ptr<WsConexion>& ws);
    int conexiones() const;
    int sensores() const;

    /** Lo mismo que hace un mensaje OSC (para las pruebas y por si otra parte del puente quiere publicar). */
    void recibir (const juce::OSCMessage& m);

    static constexpr int maxSensores = 8, maxSalidas = 8, maxConexiones = 4;

private:
    void oscMessageReceived (const juce::OSCMessage& m) override;
    void oscBundleReceived (const juce::OSCBundle& b) override;
    void hiResTimerCallback() override;

    struct Salida
    {
        juce::String id, clave;      // clave: la dirección OSC y el argumento, para el sensor «osc»
        juce::String rotulo;         // bajo el conector del estudio: 4 letras como mucho (con 8 salidas, cada una tiene 24 px)
        float valor = 0, min = 0, max = 1;
        bool cambio = false;
    };
    struct Sensor
    {
        juce::String id, nombre;
        std::vector<Salida> salidas;
        bool alta = true;            // hay que (re)mandar su alta: es nuevo o tiene una salida nueva
    };

    Sensor* sensor (const juce::String& id, const juce::String& nombre);
    Salida* salida (Sensor& s, const juce::String& id, const juce::String& clave = {}, const juce::String& rotulo = {});
    static juce::var altaDe (const Sensor& s);

    Registro& log;
    juce::OSCReceiver receptor;
    std::unique_ptr<juce::DatagramSocket> socket;
    int puertoAbierto = -1;
    bool redAbierta = false;

    mutable std::mutex m;
    std::vector<Sensor> lista;
    std::vector<std::shared_ptr<WsConexion>> destinos;
    bool hayCambios = false;
};
} // namespace synz
