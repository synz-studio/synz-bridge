; Instalador de SYNZ Bridge para Windows (Inno Setup 6). Lo compila la acción «Puente»:
;   ISCC /DVersion=1.2.0 /DOrigen=ruta\del\exe puente\instalador\synz-bridge.iss
; Se instala solo para el usuario (sin permisos de administrador) en %LocalAppData%\Programs\SYNZ Bridge.
; Sin firmar: Windows (SmartScreen) avisa de «editor desconocido» hasta que se compre un certificado.
#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Origen
  #define Origen "..\..\build\fuente\synz-puente_artefacts\Release"
#endif

[Setup]
AppId={{6F1A3C58-5B0E-4B7A-9C2D-7A5E1D3B9F10}
AppName=SYNZ Bridge
AppVersion={#Version}
AppPublisher=SYNZ
AppPublisherURL=https://synz.studio
DefaultDirName={autopf}\SYNZ Bridge
DefaultGroupName=SYNZ
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\..\dist
OutputBaseFilename=SYNZ-Bridge-Setup
SetupIconFile=..\fuente\icono\synz-bridge.ico
UninstallDisplayIcon={app}\SYNZ Bridge.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no
LicenseFile=..\LICENSE

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "arranque"; Description: "Abrir SYNZ Bridge al iniciar sesión, en segundo plano (recomendado: apenas consume y no hay que abrirlo cada vez)"; GroupDescription: "Opciones:"

[Files]
Source: "{#Origen}\SYNZ Bridge.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\SYNZ Bridge"; Filename: "{app}\SYNZ Bridge.exe"

; El enlace synz-bridge://abrir: es lo que usa el botón «Abrir» del estudio.
[Registry]
Root: HKCU; Subkey: "Software\Classes\synz-bridge"; ValueType: string; ValueData: "URL:SYNZ Bridge"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\synz-bridge"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\synz-bridge\shell\open\command"; ValueType: string; ValueData: """{app}\SYNZ Bridge.exe"" ""%1"""

[Run]
Filename: "{app}\SYNZ Bridge.exe"; Parameters: "--inicio-automatico si"; Tasks: arranque; Flags: runhidden
Filename: "{app}\SYNZ Bridge.exe"; Parameters: "--en-segundo-plano"; Description: "Abrir SYNZ Bridge ahora"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\SYNZ Bridge.exe"; Parameters: "--inicio-automatico no"; Flags: runhidden; RunOnceId: "QuitaArranque"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM ""SYNZ Bridge.exe"""; Flags: runhidden; RunOnceId: "CierraBridge"

; Antes de instalar se cierra lo que haya abierto: la versión de prueba antigua («SYNZ Puente») y cualquier SYNZ Bridge (el del zip, por ejemplo).
; Si siguen abiertos, tienen ocupado el puerto y el nuevo no puede arrancar («¿ya hay otro puente abierto?»).
[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  R: Integer;
begin
  if CurStep = ssInstall then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM "SYNZ Puente.exe"', '', SW_HIDE, ewWaitUntilTerminated, R);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM "SYNZ Bridge.exe"', '', SW_HIDE, ewWaitUntilTerminated, R);
    Sleep(800);
  end;
end;
