# El programa del puente.
juce_add_gui_app(synz-puente
  PRODUCT_NAME "SYNZ Bridge"
  COMPANY_NAME "SYNZ"
  BUNDLE_ID "studio.synz.puente"
  ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/icono/synz-bridge.png"
  ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/icono/synz-bridge.png"
  # macOS: el enlace synz-bridge://abrir (el botón ABRIR del estudio) se registra en el Info.plist del paquete
  PLIST_TO_MERGE "<key>CFBundleURLTypes</key><array><dict><key>CFBundleURLName</key><string>SYNZ Bridge</string><key>CFBundleURLSchemes</key><array><string>synz-bridge</string></array></dict></array>"
  VERSION ${PROJECT_VERSION})

target_sources(synz-puente PRIVATE
  Main.cpp
  Puente.cpp
  Instancia.cpp
  Escaner.cpp
  ${SYNZ_WS_FUENTES})

target_include_directories(synz-puente PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SYNZ_WS_INCLUDE})

if(APPLE)
  set(SYNZ_PLUGINHOST_AU 1)
else()
  set(SYNZ_PLUGINHOST_AU 0)
endif()

target_compile_definitions(synz-puente PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=${SYNZ_PLUGINHOST_AU}
  JUCE_PLUGINHOST_VST=0
  JUCE_PLUGINHOST_LADSPA=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_WEB_BROWSER=0
  JUCE_MODAL_LOOPS_PERMITTED=1
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:synz-puente,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:synz-puente,JUCE_VERSION>")

target_link_libraries(synz-puente PRIVATE
  juce::juce_audio_processors
  juce::juce_gui_basics
  juce::juce_events
  juce::juce_core
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)

# Alojar plugins AU en macOS necesita CoreAudioKit (AUGenericView); JUCE no lo añade solo al enlazar.
if(APPLE)
  target_link_libraries(synz-puente PRIVATE "-framework CoreAudioKit")
endif()

# En macOS el programa y cada plugin JUCE que aloja llevan su propia copia de JUCE. Si el programa exporta sus símbolos,
# dyld une las funciones comunes de las dos copias y, si no son idénticas, se corrompe la memoria (caída «pointer being
# freed was not allocated» al abrir el editor de un plugin hecho con JUCE). Con la visibilidad oculta cada copia usa la suya.
if(APPLE)
  set_target_properties(synz-puente PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    OBJC_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
endif()
