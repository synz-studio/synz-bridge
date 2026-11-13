// La ventana del puente: estado, registro y dos botones. Lo importante pasa en `Puente`; esto solo lo enseña.
#pragma once
#include "Puente.h"
#include "Texto.h"
#include "Bandeja.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace synz
{
class PanelPuente : public juce::Component
{
public:
    explicit PanelPuente (Puente& p) : puente (p)
    {
        addAndMakeVisible (titulo);
        titulo.setText ("SYNZ Bridge", juce::dontSendNotification);
        titulo.setFont (juce::FontOptions (20.0f, juce::Font::bold));

        addAndMakeVisible (estado);
        estado.setFont (juce::FontOptions (13.0f));

        addAndMakeVisible (registro);
        registro.setMultiLine (true);
        registro.setReadOnly (true);
        registro.setScrollbarsShown (true);
        registro.setCaretVisible (false);
        registro.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::plain));

        addAndMakeVisible (buscar);
        buscar.setButtonText ("Buscar plugins otra vez");
        buscar.onClick = [this] { puente.reescanear (true); refrescar(); };

        addAndMakeVisible (carpeta);
        carpeta.setButtonText ("Añadir carpeta de plugins…"_es);
        carpeta.onClick = [this]
        {
            selector = std::make_unique<juce::FileChooser> ("Carpeta con plugins");
            selector->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                   [this] (const juce::FileChooser& fc)
                                   {
                                       const auto r = fc.getResult();
                                       if (r != juce::File())
                                       {
                                           puente.ajustes().agregarCarpeta (r.getFullPathName());
                                           puente.registro().escribir ("Carpeta añadida: "_es + r.getFullPathName());
                                           puente.reescanear (false);
                                       }
                                   });
        };

        puente.registro().alEscribir = [this] (const juce::String& linea)
        {
            juce::Component::SafePointer<PanelPuente> w (this);     // fuera de la lambda interior: MSVC se confunde con `this` en una captura con inicializador
            juce::MessageManager::callAsync ([w, linea]
            {
                if (w != nullptr)
                    w->registro.moveCaretToEnd(), w->registro.insertTextAtCaret (linea + "\n");
            });
        };
        puente.alCambiar = [this] { refrescar(); };
        setSize (640, 420);
        refrescar();
    }

    ~PanelPuente() override
    {
        puente.registro().alEscribir = nullptr;
        puente.alCambiar = nullptr;
    }

    void refrescar()
    {
        const auto r = puente.resumen();
        juce::String t;
        t << "Escuchando en 127.0.0.1:" << puente.puerto() << "   ·   "_es << r.plugins << " plugins"
          << (r.escaneando ? " (buscando…)"_es : "") << "   ·   "_es << r.conexiones << (r.conexiones == 1 ? " web conectada" : " webs conectadas")
          << "   ·   "_es << r.instancias << " abiertos";
        if (r.oscPuerto >= 0)
            t << "   ·   OSC "_es << (r.oscRed ? "red" : "local") << ":" << r.oscPuerto << " (" << r.oscSensores << " sensores)";
        estado.setText (t, juce::dontSendNotification);
    }

    void resized() override
    {
        auto a = getLocalBounds().reduced (14);
        titulo.setBounds (a.removeFromTop (30));
        estado.setBounds (a.removeFromTop (24));
        auto abajo = a.removeFromBottom (32);
        carpeta.setBounds (abajo.removeFromRight (220));
        abajo.removeFromRight (8);
        buscar.setBounds (abajo.removeFromRight (190));
        a.removeFromBottom (8);
        registro.setBounds (a);
    }

private:
    Puente& puente;
    juce::Label titulo, estado;
    juce::TextEditor registro;
    juce::TextButton buscar, carpeta;
    std::unique_ptr<juce::FileChooser> selector;
};

class VentanaPuente : public juce::DocumentWindow
{
public:
    explicit VentanaPuente (Puente& p)
        : juce::DocumentWindow ("SYNZ Bridge", juce::Colours::darkgrey, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setIcon (iconoBridge (64));
        setContentOwned (new PanelPuente (p), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    /** Qué hacer al cerrar la ventana; si nadie lo dice, se cierra el programa. */
    std::function<void()> alCerrar;

    void closeButtonPressed() override
    {
        if (alCerrar)
            alCerrar();
        else
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};
} // namespace synz
