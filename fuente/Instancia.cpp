#if JUCE_WINDOWS || defined (_WIN32)
 #define NOMINMAX
 #include <windows.h>
#endif
#include "Instancia.h"
#include "Texto.h"
#include <algorithm>
#include <cstring>

namespace synz
{
namespace
{
    template <typename T>
    T leer (const uint8_t* p)
    {
        T v;
        std::memcpy (&v, p, sizeof (T));
        return v;
    }

    template <typename T>
    void escribir (uint8_t* p, T v)
    {
        std::memcpy (p, &v, sizeof (T));
    }

    juce::AudioChannelSet conjuntoDeCanales (int n)
    {
        if (n <= 0) return juce::AudioChannelSet::disabled();
        if (n == 1) return juce::AudioChannelSet::mono();
        if (n == 2) return juce::AudioChannelSet::stereo();
        return juce::AudioChannelSet::discreteChannels (n);
    }
}

//==============================================================================
/** La barra de título de la ventana de un plugin, con los colores y la letra de SYNZ (en vez de la del sistema). */
class LookSynz : public juce::LookAndFeel_V4
{
public:
    void drawDocumentWindowTitleBar (juce::DocumentWindow& w, juce::Graphics& g, int ancho, int alto,
                                     int espacioX, int espacioAncho, const juce::Image*, bool) override
    {
        // Como las ventanas de los instrumentos del estudio: barra oscura, nombre en mayúsculas con letra espaciada y una línea fina debajo (sin franja de color).
        g.fillAll (juce::Colour (0xff14171c));
        g.setColour (juce::Colour (0xff2c323b));
        g.fillRect (0, alto - 1, ancho, 1);
        g.setColour (juce::Colour (0xffe6e9ee));
        auto letra = juce::Font (juce::FontOptions (11.0f, juce::Font::bold));
        letra.setExtraKerningFactor (0.1f);
        g.setFont (letra);
        g.drawText (w.getName().toUpperCase(), espacioX + 12, 0, juce::jmax (10, espacioAncho - 12), alto - 1,
                    juce::Justification::centredLeft, true);
    }

    /** La esquina para agrandar: tres rayas claras, bien visibles sobre la franja oscura. */
    void drawCornerResizer (juce::Graphics& g, int ancho, int alto, bool encima, bool arrastrando) override
    {
        g.setColour (juce::Colour (0xffe6e9ee).withAlpha (encima || arrastrando ? 1.0f : 0.75f));
        for (int k = 0; k < 3; ++k)
        {
            const float d = 4.0f + 4.0f * (float) k;
            g.drawLine ((float) ancho - d - 2.0f, (float) alto - 2.0f, (float) ancho - 2.0f, (float) alto - d - 2.0f, 1.4f);
        }
    }

    juce::Button* createDocumentWindowButton (int tipo) override
    {
        if (tipo != juce::DocumentWindow::closeButton)
            return nullptr;
        juce::Path x;
        x.addLineSegment ({ 0.0f, 0.0f, 1.0f, 1.0f }, 0.16f);
        x.addLineSegment ({ 1.0f, 0.0f, 0.0f, 1.0f }, 0.16f);
        auto* b = new juce::ShapeButton ("cerrar", juce::Colour (0xffb6bcc8), juce::Colour (0xffffffff), juce::Colour (0xffe5484d));
        b->setShape (x, true, true, false);
        return b;
    }

    void positionDocumentWindowButtons (juce::DocumentWindow&, int x, int y, int ancho, int alto,
                                        juce::Button*, juce::Button*, juce::Button* cerrar, bool) override
    {
        if (cerrar != nullptr)
            cerrar->setBounds (x + ancho - 26, y + (alto - 12) / 2 + 1, 12, 12);
    }
};

/** Lo que va dentro de la ventana: el editor del plugin y, si el plugin se puede agrandar, una franja debajo para la esquina
    (la ventana del plugin es del sistema y tapa lo que se pinte encima: la esquina tiene que quedar fuera de ella). */
class Contenedor : public juce::Component, private juce::ComponentListener
{
public:
    Contenedor (juce::AudioProcessorEditor* e, int franjaAlto, std::function<void (int, int)> alCambiarElPlugin)
        : editor (e), franja (franjaAlto), pideTamano (std::move (alCambiarElPlugin))
    {
        addAndMakeVisible (editor.get());
        editor->addComponentListener (this);
    }
    ~Contenedor() override { editor->removeComponentListener (this); }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff14171c)); }

    void resized() override
    {
        const juce::ScopedValueSetter<bool> s (colocando, true);
        editor->setBounds (0, 0, getWidth(), juce::jmax (1, getHeight() - franja));
    }

    juce::AudioProcessorEditor* ed() const { return editor.get(); }

private:
    // El plugin cambió su propio tamaño (Diva lo hace al abrirse): la ventana lo sigue, para que no se corte por abajo.
    void componentMovedOrResized (juce::Component& c, bool, bool tamano) override
    {
        if (colocando || ! tamano || &c != editor.get())
            return;
        if (pideTamano && (editor->getWidth() != getWidth() || editor->getHeight() + franja != getHeight()))
            pideTamano (editor->getWidth(), editor->getHeight() + franja);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    int franja;
    std::function<void (int, int)> pideTamano;
    bool colocando = false;
};

#if JUCE_MAC
extern "C" void synzMacPonerDelante (void* vistaNativa);       // VentanaMac.mm
#endif

class Instancia::VentanaEditor : public juce::DocumentWindow
{
public:
    VentanaEditor (const juce::String& titulo, juce::AudioProcessorEditor* editor, std::function<void()> alCerrar, float escala)
        : juce::DocumentWindow (titulo, juce::Colour (0xff14171c), juce::DocumentWindow::closeButton),
          cierre (std::move (alCerrar))
    {
        setLookAndFeel (&look);
        setTitleBarHeight (26);
        setUsingNativeTitleBar (false);
        if (escala > 0.0f)
            editor->setScaleFactor (escala);
        // Solo un plugin que se deja agrandar lleva la esquina: a los de tamaño fijo (Diva, Sky Keys) no se les puede cambiar el tamaño desde fuera.
        nativo = editor->isResizable();
        const int franja = nativo ? 14 : 0;
        auto* c = new Contenedor (editor, franja, [this] (int w, int h) { siguePlugin (w, h); });
        c->setSize (juce::jmax (1, editor->getWidth()), juce::jmax (1, editor->getHeight()) + franja);
        setContentOwned (c, true);
        if (nativo)
        {
            setResizable (true, true);                                // la esquina de abajo a la derecha, como en los instrumentos de SYNZ
            setResizeLimits (320, 240, 4000, 3000);
        }
        else
            setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        // Windows no deja que un programa en segundo plano se ponga delante del navegador: la ventana nacía detrás de Chrome y
        // parecía que no se abría. Se queda «siempre delante» (por encima del navegador) mientras esté abierta.
        alFrente();
    }

    bool sePuedeAgrandar() const { return nativo; }

    ~VentanaEditor() override { setLookAndFeel (nullptr); }

    void closeButtonPressed() override
    {
        if (cierre)
            cierre();
    }

    /** Poner la ventana delante de todo, aunque el navegador tenga el foco (Windows no deja a un programa en segundo plano hacerlo sin más). */
    void alFrente()
    {
        setAlwaysOnTop (true);
       #if JUCE_MAC
        juce::Process::makeForegroundProcess();      // sin esto, un programa en segundo plano no se pone delante en macOS
       #endif
        toFront (true);
       #if JUCE_MAC
        if (auto* peer = getPeer())
            synzMacPonerDelante (peer->getNativeHandle());   // sale en el escritorio donde esté el usuario, también sobre un navegador a pantalla completa
       #endif
       #if JUCE_WINDOWS
        if (auto* peer = getPeer())
        {
            auto h = (HWND) peer->getNativeHandle();
            const DWORD hiloFrente = GetWindowThreadProcessId (GetForegroundWindow(), nullptr), hiloMio = GetCurrentThreadId();
            if (hiloFrente != 0 && hiloFrente != hiloMio)
                AttachThreadInput (hiloFrente, hiloMio, TRUE);       // para el sistema, el foco lo pide el mismo hilo que lo tiene
            ShowWindow (h, IsIconic (h) ? SW_RESTORE : SW_SHOW);
            SetWindowPos (h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            BringWindowToTop (h);
            SetForegroundWindow (h);
            if (hiloFrente != 0 && hiloFrente != hiloMio)
                AttachThreadInput (hiloFrente, hiloMio, FALSE);
            FLASHWINFO fw {};
            fw.cbSize = sizeof (fw); fw.hwnd = h; fw.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG; fw.uCount = 3;
            FlashWindowEx (&fw);                                     // si aun así no pudo, el botón de la barra de tareas parpadea
        }
       #endif
        // Se queda siempre delante: al pulsar en el navegador la ventana del plugin ya no desaparece detrás; solo se va al cerrarla (Pablo, 29 oct).
    }

    void escalar (float escala)
    {
        if (auto* c = dynamic_cast<Contenedor*> (getContentComponent()))
            if (auto* e = c->ed())
                e->setScaleFactor (escala);
    }

    /** El plugin pidió otro tamaño: la ventana se pone a su medida (más la barra de título). */
    void siguePlugin (int ancho, int alto)
    {
        setSize (ancho, alto + getTitleBarHeight());
    }

    /** Lo que se ve en la ventana. En Windows el plugin es una ventana hija del sistema que JUCE no pinta: se pide su imagen al sistema. */
    juce::Image capturar()
    {
       #if JUCE_WINDOWS
        if (auto* peer = getPeer())
        {
            auto h = (HWND) peer->getNativeHandle();
            RECT r {};
            if (h != nullptr && GetClientRect (h, &r) && r.right > 0 && r.bottom > 0)
            {
                const int w = (int) r.right, alto = (int) r.bottom;
                HDC pantalla = GetDC (h);
                HDC mem = CreateCompatibleDC (pantalla);
                BITMAPINFO bi {};
                bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -alto;          // de arriba abajo
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP bmp = CreateDIBSection (pantalla, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                juce::Image img;
                if (bmp != nullptr && bits != nullptr)
                {
                    HGDIOBJ viejo = SelectObject (mem, bmp);
                    const BOOL ok = PrintWindow (h, mem, 3 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */);
                    if (ok)
                    {
                        img = juce::Image (juce::Image::ARGB, w, alto, true);
                        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
                        for (int y = 0; y < alto; ++y)
                        {
                            auto* dst = (uint8_t*) bd.getLinePointer (y);
                            std::memcpy (dst, (const uint8_t*) bits + (size_t) y * (size_t) w * 4, (size_t) w * 4);
                            for (int x = 0; x < w; ++x)
                                dst[x * 4 + 3] = 255;     // sin transparencia
                        }
                    }
                    SelectObject (mem, viejo);
                }
                if (bmp != nullptr) DeleteObject (bmp);
                DeleteDC (mem);
                ReleaseDC (h, pantalla);
                if (img.isValid())
                    return img;
            }
        }
       #endif
       #if JUCE_MAC
    // macOS: el plugin es una vista nativa que JUCE no pinta, y la foto saldría en negro. Sin foto: el estudio dice que se ponga una imagen (IMAGEN).
    return {};
   #endif
    // Resto de sistemas (y respaldo): lo que JUCE pinta del contenido.
        // Se fotografía la ventana entera menos la barra de título: así vale también con el plugin escalado (con transformación).
        return createComponentSnapshot (getLocalBounds().withTrimmedTop (getTitleBarHeight()), true, 1.0f);
    }

private:
    LookSynz look;
    std::function<void()> cierre;
    bool nativo = false;
};

//==============================================================================
std::shared_ptr<Instancia> Instancia::crear (int id, juce::AudioPluginFormatManager& formatos,
                                             const juce::PluginDescription& d, const Pedido& pedido, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD
    std::shared_ptr<Instancia> inst (new Instancia (id));

    inst->frecuencia = juce::jlimit (8000.0, 384000.0, pedido.frecuencia);
    inst->bloqueMaximo = juce::jlimit (16, maxBloque, pedido.bloqueMax);

    inst->plugin = formatos.createPluginInstance (d, inst->frecuencia, inst->bloqueMaximo, error);
    if (inst->plugin == nullptr)
    {
        if (error.isEmpty())
            error = "el plugin no se pudo cargar";
        return nullptr;
    }

    auto& p = *inst->plugin;
    inst->nombre = d.name;
    inst->instrumento = d.isInstrument;

    // Canales: lo que pida el estudio, o lo habitual (efecto 2→2, instrumento 0→2).
    const int quieroEntradas = juce::jlimit (0, maxCanales, pedido.entradas >= 0 ? pedido.entradas : (d.isInstrument ? 0 : 2));
    const int quieroSalidas  = juce::jlimit (1, maxCanales, pedido.salidas  >= 0 ? pedido.salidas  : 2);
    auto disposicion = p.getBusesLayout();
    if (p.getBusCount (true) > 0)  disposicion.getChannelSet (true, 0)  = conjuntoDeCanales (quieroEntradas);
    if (p.getBusCount (false) > 0) disposicion.getChannelSet (false, 0) = conjuntoDeCanales (quieroSalidas);
    // Solo la entrada y la salida principales: los plugins con muchas salidas (Kontakt trae 16 estéreo, 32 canales) se quedan con el par principal.
    for (int i = 1; i < p.getBusCount (true); ++i)  disposicion.getChannelSet (true, i)  = juce::AudioChannelSet::disabled();
    for (int i = 1; i < p.getBusCount (false); ++i) disposicion.getChannelSet (false, i) = juce::AudioChannelSet::disabled();
    if (! p.setBusesLayout (disposicion))
    {
        // Si no acepta lo pedido: su disposición habitual, pero sin los buses de más.
        p.enableAllBuses();
        for (int i = 1; i < p.getBusCount (true); ++i)  if (auto* b = p.getBus (true, i))  b->enable (false);
        for (int i = 1; i < p.getBusCount (false); ++i) if (auto* b = p.getBus (false, i)) b->enable (false);
    }

    inst->entradas = p.getTotalNumInputChannels();
    inst->salidas  = juce::jmax (1, p.getTotalNumOutputChannels());
    if (inst->entradas > maxCanales || inst->salidas > maxCanales)
    {
        error = "el plugin necesita más de "_es + juce::String (maxCanales) + " canales";
        return nullptr;
    }
    inst->canalesBuffer = juce::jmax (1, inst->entradas, inst->salidas);

    p.setPlayConfigDetails (inst->entradas, inst->salidas, inst->frecuencia, inst->bloqueMaximo);
    p.prepareToPlay (inst->frecuencia, inst->bloqueMaximo);

    // Todo lo del hilo de audio se reserva ahora.
    inst->muestras.assign ((size_t) (inst->canalesBuffer * inst->bloqueMaximo), 0.0f);
    inst->punteros.resize ((size_t) inst->canalesBuffer);
    for (int c = 0; c < inst->canalesBuffer; ++c)
        inst->punteros[(size_t) c] = inst->muestras.data() + (size_t) (c * inst->bloqueMaximo);
    inst->midi.ensureSize (8192);

    inst->editor = p.hasEditor();

    const auto& todos = p.getParameters();
    inst->posicionVisible.assign ((size_t) todos.size(), -1);
    for (int i = 0; i < todos.size(); ++i)
        if (todos[i]->isAutomatable())
        {
            inst->posicionVisible[(size_t) i] = (int) inst->visibles.size();
            inst->visibles.push_back (i);
        }

    const size_t n = inst->visibles.size();
    inst->pendiente = std::vector<std::atomic<float>> (n);
    inst->sucio     = std::vector<std::atomic<bool>> (n);
    inst->puesto    = std::vector<std::atomic<float>> (n);
    for (size_t k = 0; k < n; ++k)
    {
        inst->pendiente[k] = 0.0f;
        inst->sucio[k] = false;
        inst->puesto[k] = -1.0f;
        todos[inst->visibles[k]]->addListener (inst.get());
    }
    return inst;
}

Instancia::Instancia (int idNuevo) : id (idNuevo) {}

Instancia::~Instancia()
{
    jassert (plugin == nullptr);    // `liberar()` se llamó antes, en el hilo de mensajes
}

void Instancia::liberar()
{
    JUCE_ASSERT_MESSAGE_THREAD
    ventana.reset();
    std::lock_guard<std::mutex> g (proceso);     // espera a que el hilo de audio termine el bloque en curso
    if (plugin != nullptr)
    {
        for (int real : visibles)
            plugin->getParameters()[real]->removeListener (this);
        plugin->releaseResources();
        plugin.reset();
    }
}

//==============================================================================
namespace
{
    juce::String identificador (juce::AudioProcessorParameter* p, int indice)
    {
        if (auto* alojado = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
            return alojado->getParameterID();           // el id del propio plugin (VST3, AU…), estable entre sesiones
        if (auto* conId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            return conId->paramID;
        return juce::String (indice);
    }
}

juce::var Instancia::parametros() const
{
    juce::Array<juce::var> lista;
    if (plugin == nullptr)
        return lista;
    for (size_t k = 0; k < visibles.size(); ++k)
    {
        auto* p = plugin->getParameters()[visibles[k]];
        auto* o = new juce::DynamicObject();
        o->setProperty ("i", (int) k);
        o->setProperty ("id", identificador (p, visibles[k]));
        o->setProperty ("nombre", p->getName (64));
        o->setProperty ("valor", (double) p->getValue());
        o->setProperty ("porDefecto", (double) p->getDefaultValue());
        o->setProperty ("etiqueta", p->getLabel());
        o->setProperty ("pasos", p->isDiscrete() ? p->getNumSteps() : 0);
        o->setProperty ("automatizable", true);
        lista.add (juce::var (o));
    }
    return lista;
}

juce::var Instancia::valores() const
{
    juce::Array<juce::var> lista;
    if (plugin != nullptr)
        for (int real : visibles)
            lista.add ((double) plugin->getParameters()[real]->getValue());
    return lista;
}

juce::var Instancia::describir() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("instancia", id);
    o->setProperty ("ticket", ticket);
    o->setProperty ("nombre", nombre);
    o->setProperty ("instrumento", instrumento);
    o->setProperty ("entradas", entradas);
    o->setProperty ("salidas", salidas);
    o->setProperty ("latencia", plugin != nullptr ? juce::jmax (0, plugin->getLatencySamples()) : 0);
    o->setProperty ("editor", editor);
    o->setProperty ("parametros", parametros());
    return juce::var (o);
}

bool Instancia::ponerParametro (int indice, float valor)
{
    if (plugin == nullptr || indice < 0 || (size_t) indice >= visibles.size())
        return false;
    valor = juce::jlimit (0.0f, 1.0f, valor);
    puesto[(size_t) indice] = valor;
    plugin->getParameters()[visibles[(size_t) indice]]->setValue (valor);
    return true;
}

void Instancia::parameterValueChanged (int indiceReal, float valor)
{
    // Puede llegar de cualquier hilo. Solo se anota; el temporizador del hilo de mensajes lo manda.
    if (indiceReal < 0 || (size_t) indiceReal >= posicionVisible.size())
        return;
    const int k = posicionVisible[(size_t) indiceReal];
    if (k < 0)
        return;
    if (std::abs (puesto[(size_t) k].load() - valor) < 1.0e-6f)
        return;                         // es el eco de lo que acaba de poner el estudio
    pendiente[(size_t) k] = valor;
    sucio[(size_t) k] = true;
    hayCambios = true;
}

bool Instancia::recogerCambios (std::vector<std::pair<int, float>>& destino)
{
    if (! hayCambios.exchange (false))
        return false;
    bool alguno = false;
    for (size_t i = 0; i < sucio.size(); ++i)
        if (sucio[i].exchange (false))
        {
            destino.emplace_back ((int) i, pendiente[i].load());
            alguno = true;
        }
    return alguno;
}

//==============================================================================
bool Instancia::abrirEditor (std::function<void()> alCerrar, float escala)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (plugin == nullptr)
        return false;
    if (ventana != nullptr)
    {
        if (escala > 0.0f)
            ventana->escalar (escala);
        ventana->alFrente();
        return true;
    }
    if (auto* e = plugin->createEditorIfNeeded())
    {
        ventana = std::make_unique<VentanaEditor> (nombre, e, std::move (alCerrar), escala);
        return true;
    }
    return false;
}

bool Instancia::escalarEditor (float escala)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (ventana == nullptr || ! (escala > 0.0f))
        return false;
    ventana->escalar (escala);
    return true;
}

bool Instancia::capturarEditor (juce::String& jpegBase64, int& ancho, int& alto)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (ventana == nullptr)
        return false;
    auto img = ventana->capturar();
    if (! img.isValid() || img.getWidth() < 16 || img.getHeight() < 16)
        return false;
    if (img.getWidth() > 480)
        img = img.rescaled (480, juce::jmax (1, img.getHeight() * 480 / img.getWidth()), juce::Graphics::highResamplingQuality);
    ancho = img.getWidth();
    alto = img.getHeight();
    juce::MemoryOutputStream salida;
    juce::JPEGImageFormat jpeg;
    jpeg.setQuality (0.85f);
    if (! jpeg.writeImageToStream (img, salida))
        return false;
    jpegBase64 = juce::Base64::toBase64 (salida.getData(), salida.getDataSize());
    return true;
}

void Instancia::cerrarEditor()
{
    JUCE_ASSERT_MESSAGE_THREAD
    ventana.reset();
}

juce::String Instancia::leerEstado()
{
    juce::MemoryBlock mb;
    if (plugin != nullptr)
        plugin->getStateInformation (mb);
    return juce::Base64::toBase64 (mb.getData(), mb.getSize());
}

bool Instancia::ponerEstado (const juce::String& base64)
{
    if (plugin == nullptr)
        return false;
    juce::MemoryOutputStream salida;
    if (! juce::Base64::convertFromBase64 (salida, base64))
        return false;
    plugin->setStateInformation (salida.getData(), (int) salida.getDataSize());
    return true;
}

//==============================================================================
Instancia::Fallo Instancia::procesar (const uint8_t* pet, size_t n, std::vector<uint8_t>& resp)
{
    if (n < 16)
        return Fallo::PeticionMalFormada;

    const auto secuencia = leer<uint32_t> (pet + 0);
    const int canales    = leer<uint16_t> (pet + 4);
    const int frames     = leer<uint16_t> (pet + 6);
    const int nMidi      = leer<uint16_t> (pet + 8);
    const int banderas   = leer<uint16_t> (pet + 10);

    if (canales > maxCanales || frames < 1 || frames > bloqueMaximo || nMidi > maxMidiPorBloque)
        return Fallo::PeticionMalFormada;
    if (n != 16 + (size_t) nMidi * 8 + (size_t) canales * (size_t) frames * 4)
        return Fallo::PeticionMalFormada;

    const uint8_t* bytesMidi  = pet + 16;
    const uint8_t* bytesAudio = bytesMidi + (size_t) nMidi * 8;

    // Eventos MIDI: se descartan los que no son mensajes de canal; el resto se ordena por muestra.
    int ne = 0;
    for (int k = 0; k < nMidi; ++k)
    {
        const uint8_t* e = bytesMidi + (size_t) k * 8;
        const uint8_t estado = e[2];
        if (estado < 0x80 || estado >= 0xF0)
            continue;
        const int desp = juce::jmin ((int) leer<uint16_t> (e), frames - 1);
        const uint8_t longitud = (estado >= 0xC0 && estado < 0xE0) ? 2 : 3;
        EventoMidi ev { (uint16_t) desp, estado, (uint8_t) (e[3] & 0x7f), (uint8_t) (e[4] & 0x7f), longitud };
        int j = ne++;
        while (j > 0 && eventos[j - 1].desplazamiento > ev.desplazamiento)
        {
            eventos[j] = eventos[j - 1];
            --j;
        }
        eventos[j] = ev;
    }

    const size_t bytesSalida = (size_t) salidas * (size_t) frames * 4;
    resp.resize (16 + bytesSalida);
    uint8_t* cab = resp.data();
    uint8_t* audioSalida = cab + 16;
    uint16_t banderasSalida = 0;
    uint16_t carga = 0;
    uint32_t latencia = 0;

    {
        std::lock_guard<std::mutex> g (proceso);

        if (plugin == nullptr || caido.load())
        {
            std::memset (audioSalida, 0, bytesSalida);
            banderasSalida |= 2;
        }
        else
        {
            // Entrada: los canales que lleguen; un solo canal alimenta a todos los del plugin.
            for (int c = 0; c < canalesBuffer; ++c)
            {
                float* dst = punteros[(size_t) c];
                if (c < entradas && canales > 0)
                {
                    const int canalOrigen = (c < canales) ? c : (canales == 1 ? 0 : -1);
                    if (canalOrigen >= 0)
                    {
                        std::memcpy (dst, bytesAudio + (size_t) canalOrigen * (size_t) frames * 4, (size_t) frames * 4);
                        continue;
                    }
                }
                std::memset (dst, 0, (size_t) frames * 4);
            }

            if ((banderas & 1) != 0)
            {
                // Saltar el plugin: sale lo que entró (los canales que sobran, en silencio).
                for (int c = 0; c < salidas; ++c)
                {
                    if (c < canales)
                        std::memcpy (audioSalida + (size_t) c * (size_t) frames * 4, bytesAudio + (size_t) c * (size_t) frames * 4, (size_t) frames * 4);
                    else
                        std::memset (audioSalida + (size_t) c * (size_t) frames * 4, 0, (size_t) frames * 4);
                }
            }
            else
            {
                midi.clear();
                for (int k = 0; k < ne; ++k)
                {
                    const auto& e = eventos[k];
                    const uint8_t bytes[3] = { e.estado, e.d1, e.d2 };
                    midi.addEvent (juce::MidiMessage (bytes, e.longitud), e.desplazamiento);
                }

                juce::AudioBuffer<float> buffer (punteros.data(), canalesBuffer, frames);
                juce::ScopedNoDenormals sinDenormales;
                const auto t0 = juce::Time::getHighResolutionTicks();
                try
                {
                    const juce::ScopedLock cerrojo (plugin->getCallbackLock());
                    plugin->processBlock (buffer, midi);
                }
                catch (...)
                {
                    caido = true;
                }
                const double usado = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                const double disponible = (double) frames / frecuencia;
                carga = (uint16_t) juce::jlimit (0.0, 65535.0, usado / disponible * 10000.0);
                if (usado > disponible)
                    banderasSalida |= 1;

                if (caido.load())
                {
                    std::memset (audioSalida, 0, bytesSalida);
                    banderasSalida |= 2;
                }
                else
                {
                    for (int c = 0; c < salidas; ++c)
                        std::memcpy (audioSalida + (size_t) c * (size_t) frames * 4, punteros[(size_t) c], (size_t) frames * 4);
                    latencia = (uint32_t) juce::jmax (0, plugin->getLatencySamples());
                }
            }
        }
    }

    escribir<uint32_t> (cab + 0, secuencia);
    escribir<uint16_t> (cab + 4, (uint16_t) salidas);
    escribir<uint16_t> (cab + 6, (uint16_t) frames);
    escribir<uint32_t> (cab + 8, latencia);
    escribir<uint16_t> (cab + 12, carga);
    escribir<uint16_t> (cab + 14, banderasSalida);
    return Fallo::Ninguno;
}
} // namespace synz
