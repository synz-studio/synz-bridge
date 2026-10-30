// SPDX-License-Identifier: AGPL-3.0-only
// Que SYNZ Bridge arranque solo al iniciar sesión, en segundo plano, y el iconito de la bandeja para abrirlo o cerrarlo.
#pragma once
#include "Texto.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace synz
{
/** La entrada de «abrir al iniciar sesión»: Windows (clave Run del usuario), macOS (LaunchAgent) y Linux (autostart). */
struct InicioAutomatico
{
    static constexpr const char* argumento = "--en-segundo-plano";

    static bool activo()
    {
       #if JUCE_WINDOWS
        return juce::WindowsRegistry::valueExists (claveWindows());
       #else
        return archivo().existsAsFile();
       #endif
    }

    static bool poner (bool quiero, const juce::File& programa)
    {
       #if JUCE_WINDOWS
        if (! quiero)
        {
            if (juce::WindowsRegistry::valueExists (claveWindows()))
                juce::WindowsRegistry::deleteValue (claveWindows());
            return true;
        }
        return juce::WindowsRegistry::setValue (claveWindows(), "\"" + programa.getFullPathName() + "\" " + argumento);
       #else
        auto f = archivo();
        if (! quiero)
            return ! f.existsAsFile() || f.deleteFile();
        if (! f.getParentDirectory().createDirectory().wasOk())
            return false;
       #if JUCE_MAC
        const juce::String t =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "<key>Label</key><string>studio.synz.puente</string>\n"
            "<key>ProgramArguments</key><array><string>" + programa.getFullPathName().replace ("&", "&amp;") + "</string><string>" + argumento + "</string></array>\n"
            "<key>RunAtLoad</key><true/>\n"
            "</dict></plist>\n";
       #else
        const juce::String t =
            "[Desktop Entry]\nType=Application\nName=SYNZ Bridge\nComment=Plugins del ordenador dentro de SYNZ Studio\n"
            "Exec=\"" + programa.getFullPathName() + "\" " + argumento + "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
       #endif
        return f.replaceWithText (t);
       #endif
    }

private:
   #if JUCE_WINDOWS
    static juce::String claveWindows() { return "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\SYNZ Bridge"; }
   #else
    static juce::File archivo()
    {
        const auto casa = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
       #if JUCE_MAC
        return casa.getChildFile ("Library/LaunchAgents/studio.synz.puente.plist");
       #else
        return casa.getChildFile (".config/autostart/synz-puente.desktop");
       #endif
    }
   #endif
};

/** El enlace `synz-bridge://abrir`: el estudio lo usa para abrir el programa desde el navegador. Se registra la primera vez que el programa se ejecuta.
    Windows: clave del usuario en el registro. Linux: un .desktop con el esquema. macOS: pendiente (necesita el Info.plist del paquete). */
struct EnlaceWeb
{
    static constexpr const char* esquema = "synz-bridge";

    static bool registrar (const juce::File& programa)
    {
       #if JUCE_WINDOWS
        const juce::String base = "HKEY_CURRENT_USER\\Software\\Classes\\synz-bridge";
        return juce::WindowsRegistry::setValue (base + "\\", "URL:SYNZ Bridge")
            && juce::WindowsRegistry::setValue (base + "\\URL Protocol", "")
            && juce::WindowsRegistry::setValue (base + "\\shell\\open\\command\\", "\"" + programa.getFullPathName() + "\" \"%1\"");
       #elif JUCE_LINUX
        const auto f = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share/applications/synz-bridge.desktop");
        const juce::String t = "[Desktop Entry]\nType=Application\nName=SYNZ Bridge\nNoDisplay=true\n"
                               "Exec=\"" + programa.getFullPathName() + "\" %u\nMimeType=x-scheme-handler/synz-bridge;\nTerminal=false\n";
        if (f.existsAsFile() && f.loadFileAsString() == t)
            return true;
        if (! f.getParentDirectory().createDirectory().wasOk() || ! f.replaceWithText (t))
            return false;
        juce::ChildProcess p;
        if (p.start (juce::StringArray { "xdg-mime", "default", "synz-bridge.desktop", "x-scheme-handler/synz-bridge" }))
            p.waitForProcessToFinish (3000);
        return true;
       #else
        juce::ignoreUnused (programa);
        return false;
       #endif
    }
};

/** El icono de SYNZ Bridge, dibujado igual que `icono/synz-bridge.svg`: la onda de SYNZ (la de la barra del estudio) y «VST» en un cuadrado oscuro. */
inline juce::Image iconoBridge (int lado)
{
    juce::Image img (juce::Image::ARGB, lado, lado, true);
    juce::Graphics g (img);
    const float e = (float) lado / 1024.0f;
    g.setColour (juce::Colour (0xff06080f));
    g.fillRoundedRectangle (0.0f, 0.0f, (float) lado, (float) lado, 224.0f * e);

    // La onda de la barra del estudio (M1 7 C 3 1, 6 1, 9 7 S 15 13, 17 7; trazo 1,8 de 18), ampliada x42
    juce::Path onda;
    onda.startNewSubPath (176 * e, 380 * e);
    onda.cubicTo (260 * e, 128 * e, 386 * e, 128 * e, 512 * e, 380 * e);
    onda.cubicTo (638 * e, 632 * e, 764 * e, 632 * e, 848 * e, 380 * e);
    g.setColour (juce::Colour (0xff6d74f5));
    g.strokePath (onda, juce::PathStrokeType (76 * e, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (juce::Colour (0xffe6e9ee));
    g.setFont (juce::FontOptions (290.0f * e, juce::Font::bold));
    g.drawText ("VST", juce::Rectangle<float> (0.0f, 620.0f * e, (float) lado, 300.0f * e), juce::Justification::centred, false);
    return img;
}

/** El icono de la bandeja del sistema: clic = enseñar la ventana; clic derecho = menú (mostrar, abrir al iniciar, salir). */
class IconoBandeja : public juce::SystemTrayIconComponent
{
public:
    IconoBandeja (juce::File programaActual, std::function<void()> mostrar, std::function<void()> salir)
        : programa (std::move (programaActual)), alMostrar (std::move (mostrar)), alSalir (std::move (salir))
    {
        const auto img = iconoBridge (64);
        setIconImage (img, img);
        setIconTooltip ("SYNZ Bridge");
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Abrir SYNZ Bridge"_es);
            m.addSeparator();
            m.addItem (2, "Abrir al iniciar sesión (recomendado)"_es, true, InicioAutomatico::activo());
            m.addSeparator();
            m.addItem (3, "Salir"_es);
            m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r)
            {
                if (r == 1 && alMostrar) alMostrar();
                else if (r == 2) InicioAutomatico::poner (! InicioAutomatico::activo(), programa);
                else if (r == 3 && alSalir) alSalir();
            });
        }
        else if (alMostrar)
            alMostrar();
    }

private:
    juce::File programa;
    std::function<void()> alMostrar, alSalir;
};
} // namespace synz
