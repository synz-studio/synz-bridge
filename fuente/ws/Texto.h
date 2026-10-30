// Literales de texto con tildes y eñes. `juce::String ("canción")` lee los bytes como Latin-1 y los estropea;
// con `"canción"_es` se leen como UTF-8, que es como están guardados los archivos.
#pragma once
#include <juce_core/juce_core.h>

inline juce::String operator""_es (const char* texto, std::size_t n)
{
    return juce::String::fromUTF8 (texto, (int) n);
}
