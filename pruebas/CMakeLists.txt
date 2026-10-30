juce_add_console_app(ws-eco PRODUCT_NAME "ws-eco")
target_sources(ws-eco PRIVATE ws-eco/Main.cpp ${SYNZ_WS_FUENTES})
target_include_directories(ws-eco PRIVATE ${SYNZ_WS_INCLUDE})
target_compile_definitions(ws-eco PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ws-eco PRIVATE
  juce::juce_core
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)

# Plugins VST3 de prueba (un mismo código, tres productos). Se copian a <build>/vst3-pruebas/.
function(synz_plugin_prueba nombre tipo codigo es_sinte)
  juce_add_plugin(${nombre}
    PRODUCT_NAME "${nombre}"
    COMPANY_NAME "SYNZ"
    PLUGIN_MANUFACTURER_CODE Synz
    PLUGIN_CODE ${codigo}
    FORMATS VST3
    IS_SYNTH ${es_sinte}
    NEEDS_MIDI_INPUT ${es_sinte}
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_COPY_DIR "${CMAKE_BINARY_DIR}/vst3-pruebas")
  target_sources(${nombre} PRIVATE plugins/Plugin.cpp)
  target_compile_definitions(${nombre} PRIVATE
    SYNZ_TIPO=${tipo}
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(${nombre} PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
endfunction()

synz_plugin_prueba(SynzPruebaSinte  1 Sin1 TRUE)
synz_plugin_prueba(SynzPruebaEfecto 2 Efe1 FALSE)
synz_plugin_prueba(SynzPruebaRaro   3 Rar1 FALSE)
