// SPDX-License-Identifier: AGPL-3.0-only
// SYNZ Bridge: aloja plugins nativos (VST3, AU) para el estudio web. Ver README.md y PROTOCOLO.md.
//
// Opciones de línea de órdenes (todas opcionales):
//   --puerto N               puerto de escucha (47821 por defecto; 0 = que lo elija el sistema, para pruebas)
//   --datos CARPETA          dónde guardar ajustes, caché y registro
//   --permitir-origen URL    acepta ese origen sin preguntar (se puede repetir)
//   --carpeta-plugins RUTA   mira también esa carpeta, solo en esta ejecución (se puede repetir)
//   --sin-ventana            no muestra la ventana ni el icono de la bandeja (las ventanas de los plugins sí se abren; lo usan las pruebas)
//   --en-segundo-plano       sin ventana, solo con el icono de la bandeja (es con lo que arranca al iniciar sesión)
//   --inicio-automatico si|no  pone o quita «abrir al iniciar sesión» y sale
//   --registrar-enlace       registra el enlace synz-bridge://abrir (el estudio lo usa para abrir el programa) y sale
//   synz-bridge://…          lo que recibe cuando el estudio lo abre: arranca en segundo plano y, si ya había uno abierto, no dice nada
//   --sin-preguntar          los orígenes que no estén en la lista se rechazan sin preguntar
//   --plazo-escaneo-ms N     plazo máximo para mirar cada plugin
//   --cerrar-con-entrada     se cierra cuando se cierra su entrada estándar (lo usan las pruebas)
// Uso interno: --escanear-uno FORMATO ARCHIVO (lo lanza el propio programa para mirar un plugin en un proceso aparte).
#include "Escaner.h"
#include "Texto.h"
#include "Puente.h"
#include "Ventana.h"
#include "Bandeja.h"
#include "ws/WsServidor.h"
#include <iostream>
#include <thread>

class AppPuente : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "SYNZ Bridge"; }
    const juce::String getApplicationVersion() override { return synz::versionPuente; }
    bool moreThanOneInstanceAllowed() override          { return true; }    // el puerto ya impide dos a la vez

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();

        // Modo hijo del escáner: mira un archivo, escribe el resultado y sale.
        if (args.size() >= 3 && args[0] == "--escanear-uno")
        {
            setApplicationReturnValue (synz::escanearArchivoYEscribir (args[1], args[2]));
            quit();
            return;
        }

        synz::ignorarSigpipe();

        synz::Puente::Opciones o;
        o.programa = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
       #if JUCE_MAC
        // En macOS JUCE da ~/Library (no ~/Library/Application Support) como «datos de aplicación del usuario».
        o.carpetaDatos = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/SYNZ Bridge");
       #else
        o.carpetaDatos = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SYNZ Bridge");
       #endif
        bool sinVentana = false, cerrarConEntrada = false, enSegundoPlano = false, abiertoPorEnlace = false;

        for (int i = 0; i < args.size(); ++i)
        {
            const auto sig = [&] { return i + 1 < args.size() ? args[++i] : juce::String(); };
            const auto& a = args[i];
            if (a == "--puerto")                  o.puerto = sig().getIntValue();
            else if (a == "--datos")              o.carpetaDatos = juce::File::getCurrentWorkingDirectory().getChildFile (sig());
            else if (a == "--permitir-origen")    o.origenesPermitidos.add (sig());
            else if (a == "--carpeta-plugins")    o.carpetasTemporales.add (juce::File::getCurrentWorkingDirectory().getChildFile (sig()).getFullPathName());
            else if (a == "--sin-ventana")        sinVentana = true;
            else if (a == "--en-segundo-plano")   enSegundoPlano = true;
            else if (a.startsWithIgnoreCase (juce::String (synz::EnlaceWeb::esquema) + ":")) { enSegundoPlano = true; abiertoPorEnlace = true; }
            else if (a == "--registrar-enlace")
            {
                const bool ok = synz::EnlaceWeb::registrar (o.programa);
                std::cout << (ok ? "ENLACE registrado" : "ENLACE error") << std::endl;
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
                return;
            }
            else if (a == "--inicio-automatico")
            {
                const bool si = sig().equalsIgnoreCase ("si");
                const bool ok = synz::InicioAutomatico::poner (si, o.programa);
                std::cout << (ok ? (si ? "INICIO-AUTOMATICO si" : "INICIO-AUTOMATICO no") : "INICIO-AUTOMATICO error") << std::endl;
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
                return;
            }
            else if (a == "--sin-preguntar")      o.preguntar = false;
            else if (a == "--plazo-escaneo-ms")   o.plazoEscaneoMs = sig().getIntValue();
            else if (a == "--cerrar-con-entrada") cerrarConEntrada = true;
        }

        puente = std::make_unique<synz::Puente> (o);
        if (! puente->iniciar())
        {
            std::cerr << "SYNZ Bridge: no se pudo abrir el puerto " << o.puerto << std::endl;
            if (abiertoPorEnlace)
            {
                quit();                       // el estudio lo abrió y ya había uno abierto: no hay nada que decir
                setApplicationReturnValue (0);
                return;
            }
            if (enSegundoPlano)
            {
                quit();                       // arranque automático o recién instalado: si ya hay uno abierto, no hay nada que decir
                setApplicationReturnValue (2);
                return;
            }
            if (! sinVentana)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SYNZ Bridge",
                                                        "SYNZ Bridge ya está abierto (mira el icono de la barra), o hay otra versión antigua abierta (\"SYNZ Puente\") ocupando el puerto "
                                                        + juce::String (o.puerto) + ".\nSi es la antigua, ciérrala desde el Administrador de tareas y vuelve a abrir esta."_es,
                                                        "Salir", nullptr, juce::ModalCallbackFunction::create ([this] (int) { quit(); }));
            else
                quit();
            setApplicationReturnValue (2);
            return;
        }

        std::cout << "LISTO " << puente->puerto() << std::endl;
        if (cerrarConEntrada)
            std::thread ([] { char c; while (std::cin.get (c)) {} juce::MessageManager::callAsync ([] { if (auto* a = juce::JUCEApplicationBase::getInstance()) a->systemRequestedQuit(); }); }).detach();
        if (! sinVentana)
        {
            synz::EnlaceWeb::registrar (o.programa);        // para que el estudio pueda abrirlo desde el navegador
            // Icono en la bandeja (donde el sistema lo tenga): desde él se abre la ventana, se activa «abrir al iniciar sesión» o se sale.
           #if JUCE_WINDOWS || JUCE_MAC
            bandeja = std::make_unique<synz::IconoBandeja> (o.programa, [this] { ensenaVentana(); }, [this] { quit(); });
           #endif
           #if JUCE_MAC
            // Con icono en la barra de menús y sin icono en el Dock, macOS lo trata como un programa de barra de menús: sus ventanas pueden
            // ponerse delante de un navegador a pantalla completa (un programa normal no puede entrar en ese escritorio).
            if (bandeja != nullptr)
                juce::Process::setDockIconVisible (false);
           #endif
            if (! enSegundoPlano || bandeja == nullptr)       // sin bandeja (Linux) no habría forma de abrirlo: se enseña la ventana
            {
                ensenaVentana();
                preguntaInicioAutomatico (o);
            }
        }
    }

    /** macOS entrega aquí el segundo arranque (icono pulsado o enlace synz-bridge:// con el programa ya abierto): el enlace no hace nada; el icono enseña la ventana. */
    void anotherInstanceStarted (const juce::String& linea) override
    {
        if (puente != nullptr && ! linea.containsIgnoreCase (synz::EnlaceWeb::esquema))
            ensenaVentana();
    }

    void systemRequestedQuit() override { quit(); }

    void shutdown() override
    {
        ventana.reset();
        if (puente != nullptr)
            puente->detener();
        puente.reset();
    }

private:
    void ensenaVentana()
    {
        if (ventana == nullptr)
        {
            ventana = std::make_unique<synz::VentanaPuente> (*puente);
            // Con icono en la bandeja, cerrar la ventana no cierra el programa (queda en segundo plano); sin icono, sí.
            if (bandeja != nullptr)
                ventana->alCerrar = [this] { juce::MessageManager::callAsync ([this] { ventana.reset(); }); };    // no se borra a sí misma en mitad de su propio clic
        }
        ventana->setVisible (true);
        ventana->toFront (true);
    }

    /** La primera vez que se abre a mano: ¿que arranque solo al iniciar sesión, en segundo plano? Se pregunta una vez. */
    void preguntaInicioAutomatico (const synz::Puente::Opciones& o)
    {
        const auto marca = o.carpetaDatos.getChildFile ("inicio-automatico-preguntado");
        if (! o.preguntar || marca.existsAsFile() || synz::InicioAutomatico::activo())
            return;
        marca.getParentDirectory().createDirectory();
        marca.replaceWithText ("1");
        const auto programa = o.programa;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("SYNZ Bridge")
                                          .withMessage ("¿Abrir SYNZ Bridge al iniciar sesión? (recomendado)\nApenas consume nada en segundo plano, y así no tienes que abrirlo cada vez que quieras usar un plugin VST en el estudio. Se queda sin ventana, con un icono pequeño en la barra, y lo puedes cambiar desde ese icono."_es)
                                          .withButton ("Sí, abrirlo solo"_es)
                                          .withButton ("No"),
                                      [programa] (int r) { if (r == 1) synz::InicioAutomatico::poner (true, programa); });
    }

    std::unique_ptr<synz::Puente> puente;
    std::unique_ptr<synz::VentanaPuente> ventana;
    std::unique_ptr<synz::IconoBandeja> bandeja;
};

#if JUCE_MAC
// En macOS cada hijo del escáner (--escanear-uno, uno por plugin) es un proceso del mismo paquete y, sin esto, sale en el Dock y «bota»
// una vez por plugin (con cientos de plugins, un bucle de botes). Se oculta del Dock antes de que arranque nada. Es lo que hace Projucer
// en su modo de línea de órdenes. Lo demás es lo mismo que START_JUCE_APPLICATION.
JUCE_CREATE_APPLICATION_DEFINE (AppPuente)

int main (int argc, char* argv[])
{
    if (argc >= 2 && juce::String (argv[1]) == "--escanear-uno")
        juce::Process::setDockIconVisible (false);

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
#else
START_JUCE_APPLICATION (AppPuente)
#endif
