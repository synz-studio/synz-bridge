// macOS: poner la ventana de un plugin delante aunque el navegador esté a pantalla completa o en otro escritorio.
#import <Cocoa/Cocoa.h>
#include <cstdio>

// Devuelve en `texto` cómo quedó la ventana, para el registro (así se ve por qué no sale delante sin tener que adivinarlo).
extern "C" void synzMacPonerDelante (void* vistaNativa, char* texto, int tamano)
{
    if (texto != nullptr && tamano > 0)
        texto[0] = 0;
    NSView* vista = (__bridge NSView*) vistaNativa;
    NSWindow* ventana = vista != nil ? [vista window] : nil;
    if (ventana == nil)
    {
        if (texto != nullptr)
            std::snprintf (texto, (size_t) tamano, "sin ventana nativa");
        return;
    }

    // En todos los escritorios, también sobre un navegador a pantalla completa (necesita que el programa no tenga icono en el Dock: ver Main.cpp).
    // macOS solo lo aplica al volver a mostrarla, por eso se oculta un instante y se vuelve a poner.
    NSWindowCollectionBehavior c = [ventana collectionBehavior];
    c &= ~(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorMoveToActiveSpace);
    c |= (NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary);
    [ventana orderOut: nil];
    [ventana setCollectionBehavior: c];
    [ventana setLevel: NSFloatingWindowLevel];
    [ventana setHidesOnDeactivate: NO];
    [NSApp activateIgnoringOtherApps: YES];
    [ventana makeKeyAndOrderFront: nil];
    [ventana orderFrontRegardless];

    if (texto != nullptr)
    {
        const NSRect m = [ventana frame];
        NSScreen* pantalla = [ventana screen];
        std::snprintf (texto, (size_t) tamano,
                       "nivel=%ld visible=%d escritorioActivo=%d tapada=%d appActiva=%d pantalla=%s marco=%.0f,%.0f %.0fx%.0f",
                       (long) [ventana level], (int) [ventana isVisible], (int) [ventana isOnActiveSpace],
                       (int) (([ventana occlusionState] & NSWindowOcclusionStateVisible) == 0), (int) [NSApp isActive],
                       pantalla != nil ? [[pantalla localizedName] UTF8String] : "ninguna",
                       m.origin.x, m.origin.y, m.size.width, m.size.height);
    }
}
