// Plugins VST3 de prueba para comprobar el puente. Un mismo código, tres productos (SYNZ_TIPO):
//   1  SynzPruebaSinte   instrumento: seno o cuadrada a la frecuencia de la nota, con la nota empezando EXACTAMENTE en
//                        la muestra que dice el MIDI (sin envolvente, para poder medirlo).
//   2  SynzPruebaEfecto  efecto: ganancia (0 a 1 → 0 a 2, el centro es 1) y un retardo real de 64 muestras, que declara.
//                        Con el parámetro «romper» a 1 lanza una excepción al procesar.
//   3  SynzPruebaRaro    como el efecto, pero al cargarse hace lo que diga la variable de entorno
//                        SYNZ_PRUEBA_RARO: «abortar» (se cae el proceso) o «colgar» (no vuelve nunca).
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <stdexcept>

#ifndef SYNZ_TIPO
 #error "falta SYNZ_TIPO"
#endif

namespace
{
constexpr int retardoMuestras = 64;
}

class ProcesadorPrueba : public juce::AudioProcessor
{
public:
    ProcesadorPrueba()
        : juce::AudioProcessor (propiedadesDeBuses()),
          estado (*this, nullptr, "ESTADO", crearParametros())
    {
       #if SYNZ_TIPO == 3
        if (const char* modo = std::getenv ("SYNZ_PRUEBA_RARO"))
        {
            const juce::String m (modo);
            if (m == "abortar")
                std::abort();
            if (m == "colgar")
                juce::Thread::sleep (120000);
        }
       #endif
        if (SYNZ_TIPO != 1)
            setLatencySamples (retardoMuestras);
    }

    const juce::String getName() const override
    {
       #if SYNZ_TIPO == 1
        return "SynzPruebaSinte";
       #elif SYNZ_TIPO == 2
        return "SynzPruebaEfecto";
       #else
        return "SynzPruebaRaro";
       #endif
    }

    void prepareToPlay (double frecuencia, int) override
    {
        sr = frecuencia;
        fase = 0.25;        // el seno arranca en su máximo: la primera muestra de la nota ya no es cero
        activa = false;
        for (auto& c : anillo)
            c.assign ((size_t) retardoMuestras, 0.0f);
        posicion = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals sinDenormales;
       #if SYNZ_TIPO == 1
        procesarSinte (b, midi);
       #else
        juce::ignoreUnused (midi);
        procesarEfecto (b);
       #endif
    }

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        if (l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()
            && l.getMainOutputChannelSet() != juce::AudioChannelSet::mono())
            return false;
       #if SYNZ_TIPO != 1
        return l.getMainInputChannelSet() == l.getMainOutputChannelSet();
       #else
        return true;
       #endif
    }

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return SYNZ_TIPO == 1; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destino) override
    {
        auto s = estado.copyState();
        if (auto xml = s.createXml())
            copyXmlToBinary (*xml, destino);
    }
    void setStateInformation (const void* datos, int n) override
    {
        if (auto xml = getXmlFromBinary (datos, n))
            if (xml->hasTagName (estado.state.getType()))
                estado.replaceState (juce::ValueTree::fromXml (*xml));
    }

private:
    static BusesProperties propiedadesDeBuses()
    {
       #if SYNZ_TIPO == 1
       {
        // Como Kontakt: con SYNZ_PRUEBA_SALIDAS=n trae n salidas estéreo de más, activadas (para probar que el puente se queda con la principal).
        auto b = BusesProperties().withOutput ("Salida", juce::AudioChannelSet::stereo(), true);
        const int extra = juce::SystemStats::getEnvironmentVariable ("SYNZ_PRUEBA_SALIDAS", "0").getIntValue();
        for (int i = 0; i < juce::jlimit (0, 16, extra); ++i)
            b = b.withOutput ("Salida " + juce::String (i + 2), juce::AudioChannelSet::stereo(), true);
        return b;
       }
       #else
        return BusesProperties().withInput ("Entrada", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Salida", juce::AudioChannelSet::stereo(), true);
       #endif
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout crearParametros()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "ganancia", 1 }, "Ganancia",
                                                            juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
       #if SYNZ_TIPO == 1
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "tipo", 1 }, "Tipo",
                                                             juce::StringArray { "seno", "cuadrada" }, 0));
       #else
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "romper", 1 }, "Romper", false));
       #endif
        return l;
    }

    //==========================================================================
    void procesarSinte (juce::AudioBuffer<float>& b, juce::MidiBuffer& midi)
    {
        b.clear();
        const float ganancia = *estado.getRawParameterValue ("ganancia");
        const bool cuadrada = *estado.getRawParameterValue ("tipo") > 0.5f;
        const int total = b.getNumSamples();
        int pos = 0;

        auto generar = [&] (int hasta)
        {
            for (; pos < hasta; ++pos)
            {
                if (! activa)
                    continue;
                const double s = cuadrada ? (fase < 0.5 ? 1.0 : -1.0) : std::sin (juce::MathConstants<double>::twoPi * fase);
                const float v = (float) (s * ganancia * velocidad);
                for (int c = 0; c < b.getNumChannels(); ++c)
                    b.setSample (c, pos, v);
                fase += frecuenciaNota / sr;
                fase -= std::floor (fase);
            }
        };

        for (const auto metadatos : midi)
        {
            generar (juce::jlimit (0, total, metadatos.samplePosition));
            const auto m = metadatos.getMessage();
            if (m.isNoteOn())
            {
                nota = m.getNoteNumber();
                velocidad = m.getFloatVelocity();
                frecuenciaNota = juce::MidiMessage::getMidiNoteInHertz (nota);
                fase = 0.25;
                activa = true;
            }
            else if (m.isNoteOff() && m.getNoteNumber() == nota)
                activa = false;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                activa = false;
        }
        generar (total);
    }

    void procesarEfecto (juce::AudioBuffer<float>& b)
    {
        if (*estado.getRawParameterValue ("romper") > 0.5f)
            throw std::runtime_error ("plugin de prueba roto a propósito");

        const float g = *estado.getRawParameterValue ("ganancia") * 2.0f;
        const int n = b.getNumSamples();
        const int canales = juce::jmin (b.getNumChannels(), (int) std::size (anillo));
        for (int c = 0; c < canales; ++c)
        {
            auto* d = b.getWritePointer (c);
            auto& a = anillo[(size_t) c];
            int p = posicion;
            for (int i = 0; i < n; ++i)
            {
                const float entrada = d[i];
                d[i] = a[(size_t) p] * g;           // sale lo que entró hace «retardoMuestras» muestras
                a[(size_t) p] = entrada;
                p = (p + 1) % retardoMuestras;
            }
        }
        posicion = (posicion + n) % retardoMuestras;
    }

    juce::AudioProcessorValueTreeState estado;
    double sr = 48000.0;
    double fase = 0.25, frecuenciaNota = 440.0;
    float velocidad = 1.0f;
    int nota = -1;
    bool activa = false;
    std::vector<float> anillo[2];
    int posicion = 0;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ProcesadorPrueba();
}
