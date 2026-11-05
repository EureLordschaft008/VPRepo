; Installer für Mediathek Downloader unter Windows (NSIS, aufgerufen von windows/build.sh)
;
; Installiert für den angemeldeten Nutzer ohne Administratorrechte nach
; %LOCALAPPDATA%\Programs\Mediathek Downloader. Mit /UPDATE startet ihn das Programm selbst:
; Er überspringt die Fragen, wartet, bis das Programm beendet ist, und ersetzt dann die Dateien.
;
; Erwartet: VERSION, SRC (fertiger Programmordner), ICON, LICENSE, OUTFILE

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APP "Mediathek Downloader"
!define EXE "mediathek-downloader.exe"
!define REGKEY "Software\Mediathek Downloader"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MediathekDownloader"

Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "${REGKEY}" "Pfad"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"

Var Update

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "${APP} starten"

!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipOnUpdate
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipOnUpdate
!insertmacro MUI_PAGE_LICENSE "${LICENSE}"
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipOnUpdate
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "German"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1031 "ProductName" "${APP}"
VIAddVersionKey /LANG=1031 "FileDescription" "${APP} – Installation"
VIAddVersionKey /LANG=1031 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1031 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1031 "LegalCopyright" "GPL-3.0-or-later"

Function .onInit
    StrCpy $Update 0
    ${GetParameters} $R0
    ClearErrors
    ${GetOptions} $R0 "/UPDATE" $R1
    IfErrors done
    StrCpy $Update 1
    ; Solange das Programm läuft, lässt sich seine .exe nicht zum Schreiben öffnen (max. 60 s)
    StrCpy $R2 0
    wait:
        IfFileExists "$INSTDIR\bin\${EXE}" 0 done
        ClearErrors
        FileOpen $R3 "$INSTDIR\bin\${EXE}" a
        IfErrors 0 free
        Sleep 500
        IntOp $R2 $R2 + 1
        IntCmp $R2 120 done wait done
    free:
        FileClose $R3
    done:
FunctionEnd

Function SkipOnUpdate
    StrCmp $Update 1 0 +2
    Abort
FunctionEnd

; Nur eigene Unterordner löschen: Der Nutzer kann einen beliebigen Ordner gewählt haben.
!macro RemoveAppFiles
    RMDir /r "$INSTDIR\bin"
    RMDir /r "$INSTDIR\lib"
    RMDir /r "$INSTDIR\libexec"
    RMDir /r "$INSTDIR\share"
    RMDir /r "$INSTDIR\etc"
    Delete "$INSTDIR\LICENSE.txt"
!macroend

Section "Programm"
    SetOutPath "$INSTDIR"
    ; alte Bibliotheken einer früheren Version entfernen, damit keine Reste mitladen
    IfFileExists "$INSTDIR\bin\${EXE}" 0 fresh
    !insertmacro RemoveAppFiles
    fresh:
    File /r "${SRC}\*.*"
    WriteUninstaller "$INSTDIR\uninstall.exe"
    CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\bin\${EXE}"
    WriteRegStr HKCU "${REGKEY}" "Pfad" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APP}"
    WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\bin\${EXE}"
    WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "EureLordschaft008"
    WriteRegStr HKCU "${UNINSTKEY}" "URLInfoAbout" \
        "https://github.com/EureLordschaft008/mediathek-downloader"
    WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" $0
SectionEnd

; Einstellungen, Verlauf und Videos (%USERPROFILE%\.mediathek-downloader, Speicherort) bleiben
Section "Uninstall"
    Delete "$SMPROGRAMS\${APP}.lnk"
    !insertmacro RemoveAppFiles
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKCU "${UNINSTKEY}"
    DeleteRegKey HKCU "${REGKEY}"
SectionEnd
