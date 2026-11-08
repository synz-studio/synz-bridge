// macOS: poner la ventana de un plugin delante aunque el navegador esté a pantalla completa o en otro escritorio.
#import <Cocoa/Cocoa.h>

void synzMacPonerDelante (void* vistaNativa)
{
    NSView* vista = (__bridge NSView*) vistaNativa;
    NSWindow* ventana = vista != nil ? [vista window] : nil;
    if (ventana == nil)
        return;

    // Aparece en el escritorio (o pantalla completa) donde esté el usuario, no en el escritorio donde nació el programa.
    [ventana setCollectionBehavior: ([ventana collectionBehavior]
                                     | NSWindowCollectionBehaviorCanJoinAllSpaces
                                     | NSWindowCollectionBehaviorFullScreenAuxiliary)];
    [ventana setLevel: NSFloatingWindowLevel];
    [ventana setHidesOnDeactivate: NO];
    [NSApp activateIgnoringOtherApps: YES];
    [ventana makeKeyAndOrderFront: nil];
    [ventana orderFrontRegardless];
}
