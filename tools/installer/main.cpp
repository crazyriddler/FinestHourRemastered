// Finest Hour Remastered's installer: a small styled window (Dear ImGui on Direct3D 11) that takes the player's own
// copy of Call of Duty: Finest Hour (its files, a disc image or an XISO), checks it is the release the port was made
// from, and installs the port beside it: the game's files under assets\, the port's files from the base payload that
// ships beside the installer (FinestHourRemastered.index and its .data.NNN parts, the v1.0 release's, fixed), then
// the latest update carried inside this exe (every change since that base), shortcuts.
// Named ...Update.exe it is the updater: it brings an installation of any earlier version to the update inside it.
// Its text in the game's languages (English, French, Italian, Spanish, Portuguese), by Windows' own.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "LzmaDec.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "resource.h"
#include "xdvdfs.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// the release the port was recompiled from
constexpr char kXbeSha1[] = "938a7b1e17928de49cb3c5834d61abcd3d9081c8";
constexpr wchar_t kProduct[] = L"Finest Hour Remastered";
constexpr wchar_t kExe[] = L"CODFH.exe";
constexpr char kRequired[] =
    "Call of Duty: Finest Hour for the original Xbox, PAL (Europe) release, title ID 4156002A, version 3.";

// --- text in the game's languages -----------------------------------------------------------------------------------

int g_lang = 0;  // 0 English, 1 French, 2 Italian, 3 Spanish, 4 Portuguese: Windows' language, else English
struct Row {
    const char *en, *fr, *it, *es;
};
// UTF-8
constexpr Row kStrings[] = {
    {"A native PC port of Call of Duty: Finest Hour  -  by crazyriddler",
     "Un portage PC natif de Call of Duty: Finest Hour  -  par crazyriddler",
     "Un port nativo per PC di Call of Duty: Finest Hour  -  di crazyriddler",
     "Un port nativo para PC de Call of Duty: Finest Hour  -  por crazyriddler"},
    {"Before installing, the release's other downloads are needed in this installer's folder:",
     "Avant d'installer, les autres téléchargements de la version doivent être dans le dossier de ce programme :",
     "Prima di installare servono gli altri download della versione nella cartella di questo programma:",
     "Antes de instalar, hacen falta las demás descargas de la versión en la carpeta de este instalador:"},
    {"Missing:", "Manquant :", "Mancano:", "Faltan:"},
    {"Download them from the release page into that folder, then check again.",
     "Téléchargez-les depuis la page de la version dans ce dossier, puis vérifiez à nouveau.",
     "Scaricali dalla pagina della versione in quella cartella, poi controlla di nuovo.",
     "Descárgalas desde la página de la versión en esa carpeta y vuelve a comprobarlo."},
    {"Release page", "Page de la version", "Pagina della versione", "Página de la versión"},
    {"Check again", "Vérifier à nouveau", "Controlla di nuovo", "Comprobar de nuevo"},
    {"Welcome. This installs Finest Hour Remastered, a native Windows port of the original Xbox game with a remastered "
     "renderer, from your own copy of the game.",
     "Bienvenue. Ce programme installe Finest Hour Remastered, un portage Windows natif du jeu Xbox original avec un "
     "rendu remasterisé, à partir de votre propre copie du jeu.",
     "Benvenuto. Questo programma installa Finest Hour Remastered, un port nativo per Windows del gioco originale per "
     "Xbox con una grafica rimasterizzata, dalla tua copia del gioco.",
     "Bienvenido. Esto instala Finest Hour Remastered, un port nativo para Windows del juego original de Xbox con unos "
     "gráficos remasterizados, a partir de tu propia copia del juego."},
    {"You need:", "Il vous faut :", "Ti serve:", "Necesitas:"},
    {"Call of Duty: Finest Hour for the original Xbox, PAL (Europe) release, title ID 4156002A, version 3.",
     "Call of Duty: Finest Hour pour la Xbox originale, version PAL (Europe), title ID 4156002A, version 3.",
     "Call of Duty: Finest Hour per la Xbox originale, versione PAL (Europa), title ID 4156002A, versione 3.",
     "Call of Duty: Finest Hour para la Xbox original, versión PAL (Europa), title ID 4156002A, versión 3."},
    {"Its files (a folder with default.xbe), a disc image (.iso) or an XISO. No game files are included: they are "
     "read from your copy.",
     "Ses fichiers (un dossier avec default.xbe), une image disque (.iso) ou un XISO. Aucun fichier du jeu n'est "
     "inclus : ils sont lus depuis votre copie.",
     "I suoi file (una cartella con default.xbe), un'immagine disco (.iso) o un XISO. Non è incluso alcun file del "
     "gioco: vengono letti dalla tua copia.",
     "Sus archivos (una carpeta con default.xbe), una imagen de disco (.iso) o un XISO. No se incluye ningún archivo "
     "del juego: se leen de tu copia."},
    {"Continue", "Continuer", "Continua", "Continuar"},
    {"Back", "Retour", "Indietro", "Atrás"},
    {"Install", "Installer", "Installa", "Instalar"},
    {"Update", "Mettre à jour", "Aggiorna", "Actualizar"},
    {"Cancel", "Annuler", "Annulla", "Cancelar"},
    {"Close", "Fermer", "Chiudi", "Cerrar"},
    {"Play now", "Jouer", "Gioca ora", "Jugar ahora"},
    {"Browse...", "Parcourir...", "Sfoglia...", "Examinar..."},
    {"Where is your copy of the game?", "Où se trouve votre copie du jeu ?", "Dov'è la tua copia del gioco?",
     "¿Dónde está tu copia del juego?"},
    {"Game folder...", "Dossier du jeu...", "Cartella del gioco...", "Carpeta del juego..."},
    {"Disc image / XISO...", "Image disque / XISO...", "Immagine disco / XISO...", "Imagen de disco / XISO..."},
    {"The image has no default.xbe: it is not this game's disc.",
     "L'image n'a pas de default.xbe : ce n'est pas le disque de ce jeu.",
     "L'immagine non ha default.xbe: non è il disco di questo gioco.",
     "La imagen no tiene default.xbe: no es el disco de este juego."},
    {"The folder has no default.xbe: choose the folder that holds the game's files\n(default.xbe, COD.PAK, AUDIO, "
     "SHELL, SB_AGD, ...).",
     "Le dossier n'a pas de default.xbe : choisissez le dossier qui contient les fichiers du jeu\n(default.xbe, COD.PAK, "
     "AUDIO, SHELL, SB_AGD, ...).",
     "La cartella non ha default.xbe: scegli la cartella con i file del gioco\n(default.xbe, COD.PAK, AUDIO, SHELL, "
     "SB_AGD, ...).",
     "La carpeta no tiene default.xbe: elige la carpeta con los archivos del juego\n(default.xbe, COD.PAK, AUDIO, "
     "SHELL, SB_AGD, ...)."},
    {"This copy is a different release of the game. The port only runs the PAL (Europe) release, version 3.",
     "Cette copie est une autre version du jeu. Le portage ne fonctionne qu'avec la version PAL (Europe), version 3.",
     "Questa copia è un'altra versione del gioco. Il port funziona solo con la versione PAL (Europa), versione 3.",
     "Esta copia es otra versión del juego. El port solo funciona con la versión PAL (Europa), versión 3."},
    {"Your copy is the right release.", "Votre copie est la bonne version.", "La tua copia è la versione giusta.",
     "Tu copia es la versión correcta."},
    {"Install to:", "Installer dans :", "Installa in:", "Instalar en:"},
    {"About 9 GB: the game's files and the remastered textures.",
     "Environ 9 Go : les fichiers du jeu et les textures remasterisées.",
     "Circa 9 GB: i file del gioco e le texture rimasterizzate.",
     "Unos 9 GB: los archivos del juego y las texturas remasterizadas."},
    {"Desktop shortcut", "Raccourci sur le bureau", "Collegamento sul desktop", "Acceso directo en el escritorio"},
    {"Start menu shortcut", "Raccourci dans le menu Démarrer", "Collegamento nel menu Start",
     "Acceso directo en el menú Inicio"},
    {"Copying the game's files...", "Copie des fichiers du jeu...", "Copia dei file del gioco...",
     "Copiando los archivos del juego..."},
    {"Installing Finest Hour Remastered...", "Installation de Finest Hour Remastered...",
     "Installazione di Finest Hour Remastered...", "Instalando Finest Hour Remastered..."},
    {"Applying the latest update...", "Application de la dernière mise à jour...", "Applicazione dell'ultimo aggiornamento...",
     "Aplicando la última actualización..."},
    {"Checking the installed files...", "Vérification des fichiers installés...", "Controllo dei file installati...",
     "Comprobando los archivos instalados..."},
    {"Making shortcuts...", "Création des raccourcis...", "Creazione dei collegamenti...", "Creando los accesos directos..."},
    {"Done.", "Terminé.", "Fatto.", "Hecho."},
    {"Finest Hour Remastered is installed.", "Finest Hour Remastered est installé.", "Finest Hour Remastered è installato.",
     "Finest Hour Remastered está instalado."},
    {"In the game, choose PC Settings in the main menu (or press F10) for the display, graphics and control options. "
     "The first start prepares the shaders for your graphics card.",
     "Dans le jeu, choisissez Options PC dans le menu principal (ou appuyez sur F10) pour l'affichage, les graphismes "
     "et les commandes. Le premier lancement prépare les shaders pour votre carte graphique.",
     "Nel gioco, scegli Opzioni PC nel menu principale (o premi F10) per schermo, grafica e comandi. Il primo avvio "
     "prepara gli shader per la tua scheda grafica.",
     "En el juego, elige Opciones de PC en el menú principal (o pulsa F10) para la pantalla, los gráficos y los "
     "controles. El primer arranque prepara los shaders para tu tarjeta gráfica."},
    {"The installation did not finish.", "L'installation n'a pas abouti.", "L'installazione non è terminata.",
     "La instalación no ha terminado."},
    {"Not affiliated with Activision. Call of Duty is a trademark of Activision.",
     "Sans lien avec Activision. Call of Duty est une marque d'Activision.",
     "Non affiliato ad Activision. Call of Duty è un marchio di Activision.",
     "Sin relación con Activision. Call of Duty es una marca de Activision."},
    {"Cancelled.", "Annulé.", "Annullato.", "Cancelado."},
    {"This updates an installed Finest Hour Remastered to the latest version, whatever version it has now. Your "
     "settings and saved games are kept.",
     "Ce programme met à jour une installation de Finest Hour Remastered vers la dernière version, quelle que soit sa "
     "version actuelle. Vos réglages et sauvegardes sont conservés.",
     "Questo programma aggiorna un'installazione di Finest Hour Remastered all'ultima versione, qualunque sia la sua "
     "versione attuale. Impostazioni e salvataggi vengono mantenuti.",
     "Esto actualiza una instalación de Finest Hour Remastered a la última versión, sea cual sea la que tenga ahora. "
     "Tus ajustes y partidas guardadas se conservan."},
    {"Update:", "Mise à jour :", "Aggiornamento:", "Actualización:"},
    {"Installed:", "Installée :", "Installata:", "Instalada:"},
    {"an earlier version", "une version antérieure", "una versione precedente", "una versión anterior"},
    {"Where is Finest Hour Remastered installed?", "Où est installé Finest Hour Remastered ?",
     "Dove è installato Finest Hour Remastered?", "¿Dónde está instalado Finest Hour Remastered?"},
    {"This folder has no Finest Hour Remastered installation.",
     "Ce dossier ne contient pas d'installation de Finest Hour Remastered.",
     "Questa cartella non contiene un'installazione di Finest Hour Remastered.",
     "Esta carpeta no tiene una instalación de Finest Hour Remastered."},
    {"Updating Finest Hour Remastered...", "Mise à jour de Finest Hour Remastered...", "Aggiornamento di Finest Hour Remastered...",
     "Actualizando Finest Hour Remastered..."},
    {"Finest Hour Remastered is up to date.", "Finest Hour Remastered est à jour.", "Finest Hour Remastered è aggiornato.",
     "Finest Hour Remastered está actualizado."},
    {"The update did not finish.", "La mise à jour n'a pas abouti.", "L'aggiornamento non è terminato.",
     "La actualización no ha terminado."},
    {"Some installed files differ from the release and are not in this update. If the game misbehaves, install it "
     "again with the full release.",
     "Certains fichiers installés diffèrent de la version et ne sont pas dans cette mise à jour. Si le jeu se comporte "
     "mal, réinstallez-le avec la version complète.",
     "Alcuni file installati sono diversi dalla versione e non sono in questo aggiornamento. Se il gioco non funziona "
     "bene, reinstallalo con la versione completa.",
     "Algunos archivos instalados no coinciden con la versión y no vienen en esta actualización. Si el juego falla, "
     "vuelve a instalarlo con la versión completa."},
    {"This program has no update inside: download it again.",
     "Ce programme ne contient pas de mise à jour : téléchargez-le à nouveau.",
     "Questo programma non contiene un aggiornamento: scaricalo di nuovo.",
     "Este programa no lleva ninguna actualización dentro: vuelve a descargarlo."},
    {"Cannot write ", "Impossible d'écrire ", "Impossibile scrivere ", "No se puede escribir "},
    {"Closing Finest Hour Remastered...", "Fermeture de Finest Hour Remastered...", "Chiusura di Finest Hour Remastered...",
     "Cerrando Finest Hour Remastered..."},
    {"Finest Hour Remastered is running from this folder: close it, then try again.",
     "Finest Hour Remastered est ouvert depuis ce dossier : fermez-le, puis réessayez.",
     "Finest Hour Remastered è aperto da questa cartella: chiudilo, poi riprova.",
     "Finest Hour Remastered está abierto desde esta carpeta: ciérralo y vuelve a intentarlo."},
    {"The folder cannot be made: ", "Impossible de créer le dossier : ", "Impossibile creare la cartella: ",
     "No se puede crear la carpeta: "},
};

// Portuguese (UTF-8)
constexpr struct {
    const char *en, *pt;
} kPortuguese[] = {
    {"A native PC port of Call of Duty: Finest Hour  -  by crazyriddler",
     "Um port nativo para PC de Call of Duty: Finest Hour  -  por crazyriddler"},
    {"Before installing, the release's other downloads are needed in this installer's folder:",
     "Antes de instalar, os outros downloads da versão precisam estar na pasta deste instalador:"},
    {"Missing:",
     "Faltando:"},
    {"Download them from the release page into that folder, then check again.",
     "Baixe-os da página da versão para essa pasta e verifique de novo."},
    {"Release page",
     "Página da versão"},
    {"Check again",
     "Verificar de novo"},
    {"Welcome. This installs Finest Hour Remastered, a native Windows port of the original Xbox game "
     "with a remastered renderer, from your own copy of the game.",
     "Bem-vindo. Este programa instala o Finest Hour Remastered, um port nativo para Windows do jogo "
     "original do Xbox com gráficos remasterizados, a partir da sua própria cópia do jogo."},
    {"You need:",
     "Você precisa de:"},
    {"Call of Duty: Finest Hour for the original Xbox, PAL (Europe) release, title ID 4156002A, "
     "version 3.",
     "Call of Duty: Finest Hour para o Xbox original, versão PAL (Europa), title ID 4156002A, versão "
     "3."},
    {"Its files (a folder with default.xbe), a disc image (.iso) or an XISO. No game files are "
     "included: they are read from your copy.",
     "Seus arquivos (uma pasta com default.xbe), uma imagem de disco (.iso) ou um XISO. Nenhum "
     "arquivo do jogo vem incluído: eles são lidos da sua cópia."},
    {"Continue",
     "Continuar"},
    {"Back",
     "Voltar"},
    {"Install",
     "Instalar"},
    {"Update",
     "Atualizar"},
    {"Cancel",
     "Cancelar"},
    {"Close",
     "Fechar"},
    {"Play now",
     "Jogar agora"},
    {"Browse...",
     "Procurar..."},
    {"Where is your copy of the game?",
     "Onde está a sua cópia do jogo?"},
    {"Game folder...",
     "Pasta do jogo..."},
    {"Disc image / XISO...",
     "Imagem de disco / XISO..."},
    {"The image has no default.xbe: it is not this game's disc.",
     "A imagem não tem default.xbe: não é o disco deste jogo."},
    {"The folder has no default.xbe: choose the folder that holds the game's files\n(default.xbe, "
     "COD.PAK, AUDIO, SHELL, SB_AGD, ...).",
     "A pasta não tem default.xbe: escolha a pasta com os arquivos do jogo\n(default.xbe, COD.PAK, "
     "AUDIO, SHELL, SB_AGD, ...)."},
    {"This copy is a different release of the game. The port only runs the PAL (Europe) release, "
     "version 3.",
     "Esta cópia é outra versão do jogo. O port só funciona com a versão PAL (Europa), versão 3."},
    {"Your copy is the right release.",
     "Sua cópia é a versão certa."},
    {"Install to:",
     "Instalar em:"},
    {"About 9 GB: the game's files and the remastered textures.",
     "Cerca de 9 GB: os arquivos do jogo e as texturas remasterizadas."},
    {"Desktop shortcut",
     "Atalho na área de trabalho"},
    {"Start menu shortcut",
     "Atalho no menu Iniciar"},
    {"Copying the game's files...",
     "Copiando os arquivos do jogo..."},
    {"Installing Finest Hour Remastered...",
     "Instalando o Finest Hour Remastered..."},
    {"Applying the latest update...",
     "Aplicando a última atualização..."},
    {"Checking the installed files...",
     "Verificando os arquivos instalados..."},
    {"Making shortcuts...",
     "Criando os atalhos..."},
    {"Done.",
     "Pronto."},
    {"Finest Hour Remastered is installed.",
     "O Finest Hour Remastered está instalado."},
    {"In the game, choose PC Settings in the main menu (or press F10) for the display, graphics and "
     "control options. The first start prepares the shaders for your graphics card.",
     "No jogo, escolha Opções do PC no menu principal (ou pressione F10) para as opções de tela, "
     "gráficos e controles. A primeira execução prepara os shaders para a sua placa de vídeo."},
    {"The installation did not finish.",
     "A instalação não foi concluída."},
    {"Not affiliated with Activision. Call of Duty is a trademark of Activision.",
     "Sem vínculo com a Activision. Call of Duty é uma marca registrada da Activision."},
    {"Cancelled.",
     "Cancelado."},
    {"This updates an installed Finest Hour Remastered to the latest version, whatever version it has "
     "now. Your settings and saved games are kept.",
     "Este programa atualiza uma instalação do Finest Hour Remastered para a última versão, seja qual "
     "for a versão atual. Suas configurações e jogos salvos são mantidos."},
    {"Update:",
     "Atualização:"},
    {"Installed:",
     "Instalada:"},
    {"an earlier version",
     "uma versão anterior"},
    {"Where is Finest Hour Remastered installed?",
     "Onde o Finest Hour Remastered está instalado?"},
    {"This folder has no Finest Hour Remastered installation.",
     "Esta pasta não tem uma instalação do Finest Hour Remastered."},
    {"Updating Finest Hour Remastered...",
     "Atualizando o Finest Hour Remastered..."},
    {"Finest Hour Remastered is up to date.",
     "O Finest Hour Remastered está atualizado."},
    {"The update did not finish.",
     "A atualização não foi concluída."},
    {"Some installed files differ from the release and are not in this update. If the game "
     "misbehaves, install it again with the full release.",
     "Alguns arquivos instalados são diferentes da versão e não estão nesta atualização. Se o jogo "
     "apresentar problemas, instale-o de novo com a versão completa."},
    {"This program has no update inside: download it again.",
     "Este programa não contém nenhuma atualização: baixe-o de novo."},
    {"Cannot write ",
     "Não foi possível gravar "},
    {"Closing Finest Hour Remastered...",
     "Fechando o Finest Hour Remastered..."},
    {"Finest Hour Remastered is running from this folder: close it, then try again.",
     "O Finest Hour Remastered está aberto a partir desta pasta: feche-o e tente de novo."},
    {"The folder cannot be made: ",
     "Não foi possível criar a pasta: "},
};

// the text in the player's language (English as written when it has none)
const char* T(const char* en) {
    if (g_lang == 4) {
        for (const auto& r : kPortuguese)
            if (std::strcmp(r.en, en) == 0) return r.pt;
        return en;
    }
    for (const Row& r : kStrings)
        if (std::strcmp(r.en, en) == 0) {
            const char* t[] = {r.en, r.fr, r.it, r.es};
            return t[g_lang];
        }
    return en;
}

// Windows' language: French, Italian, Spanish or Portuguese, else English
int SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_FRENCH: return 1;
        case LANG_ITALIAN: return 2;
        case LANG_SPANISH: return 3;
        case LANG_PORTUGUESE: return 4;
        default: return 0;
    }
}

// the game's language codes (XC_LANGUAGE): 1 English, 4 French, 6 Italian, 5 Spanish, 9 Portuguese, 10 Russian (the
// port's; the installer itself in English on a Russian Windows)
uint32_t GameLanguage() {
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN) return 10;
    const uint32_t codes[] = {1, 4, 6, 5, 9};
    return codes[g_lang];
}

// --- the update inside this exe -------------------------------------------------------------------------------------
// make_release.py appends it: the packed update (LZMA, the LZMA SDK's "alone" format: 5 bytes of properties, 8 of
// size, the stream), its packed and unpacked sizes and "FHRLZMA1". Unpacked by the LZMA SDK's own decoder (LzmaDec.c),
// no part of Windows: Wine's compression API (cabinet.dll, which the LZMS packing before used) is a stub, and on Linux
// the updater found no update inside. Unpacked: a text head, then each file's bytes in the head's order.
//   FHRU 1 / version <text> / file <size> <sha1> <path> / delete <path> / check <size> <path> / end
// file: written to the installation; delete: an earlier version's file gone (the first release's exe); check: a base
// file this update leaves alone, its size compared (an installation of any earlier version is brought up to date by
// the files; one that differs elsewhere is told)

struct PatchFile {
    std::string path, sha1;
    uint64_t size = 0, at = 0;  // at: where its bytes begin in data
};
struct Patch {
    std::string version;
    std::vector<PatchFile> files;
    std::vector<std::string> deletes;
    std::vector<std::pair<uint64_t, std::string>> checks;
    std::vector<uint8_t> data;
    bool ok = false;
};
Patch g_patch;
bool g_update_mode = false;  // the updater (its exe named ...Update.exe, or --update)
std::wstring g_installed;    // the update's: the installation's version as found
size_t g_differ = 0;         // files of the installation not as the release's and not in the update

bool ReadPatch(Patch& p) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    FILE* f = _wfopen(self, L"rb");
    if (!f) return false;
    uint8_t tail[24];
    _fseeki64(f, -24, SEEK_END);
    int64_t end = _ftelli64(f);
    bool ok = std::fread(tail, 1, 24, f) == 24 && std::memcmp(tail + 16, "FHRLZMA1", 8) == 0;
    uint64_t packed = 0, raw = 0;
    if (ok) std::memcpy(&packed, tail, 8), std::memcpy(&raw, tail + 8, 8);
    ok = ok && packed > 0 && packed <= uint64_t(end) && raw < (uint64_t(1) << 33);
    std::vector<uint8_t> blob;
    if (ok) {
        blob.resize(size_t(packed));
        _fseeki64(f, end - int64_t(packed), SEEK_SET);
        ok = std::fread(blob.data(), 1, blob.size(), f) == blob.size();
    }
    std::fclose(f);
    if (!ok) return false;
    if (blob.size() <= LZMA_PROPS_SIZE + 8) return false;
    p.data.resize(size_t(raw));
    const ISzAlloc alloc = {[](ISzAllocPtr, size_t n) { return std::malloc(n); }, [](ISzAllocPtr, void* q) { std::free(q); }};
    SizeT got = SizeT(raw), in = SizeT(blob.size() - LZMA_PROPS_SIZE - 8);
    ELzmaStatus status;
    ok = LzmaDecode(p.data.data(), &got, blob.data() + LZMA_PROPS_SIZE + 8, &in, blob.data(), LZMA_PROPS_SIZE, LZMA_FINISH_END,
                    &status, &alloc) == SZ_OK &&
         got == raw;
    if (!ok) return false;
    // the head: lines up to "end"
    size_t at = 0;
    auto line = [&](std::string& out) {
        size_t nl = std::find(p.data.begin() + ptrdiff_t(at), p.data.end(), '\n') - p.data.begin();
        if (nl >= p.data.size()) return false;
        out.assign(reinterpret_cast<const char*>(p.data.data()) + at, nl - at);
        at = nl + 1;
        return true;
    };
    std::string l;
    if (!line(l) || l != "FHRU 1") return false;
    std::vector<PatchFile> files;
    while (line(l) && l != "end") {
        std::istringstream s(l);
        std::string kind;
        s >> kind;
        std::string rest;
        if (kind == "version") std::getline(s >> std::ws, p.version);
        else if (kind == "file") {
            PatchFile pf;
            s >> pf.size >> pf.sha1;
            std::getline(s >> std::ws, pf.path);
            files.push_back(pf);
        } else if (kind == "delete") std::getline(s >> std::ws, rest), p.deletes.push_back(rest);
        else if (kind == "check") {
            uint64_t size = 0;
            s >> size;
            std::getline(s >> std::ws, rest);
            p.checks.push_back({size, rest});
        }
    }
    if (l != "end") return false;
    for (PatchFile& pf : files) {
        pf.at = at;
        at += pf.size;
    }
    if (at > p.data.size()) return false;
    p.files = std::move(files);
    p.ok = true;
    return true;
}

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11ShaderResourceView> g_art;
int g_art_w = 0, g_art_h = 0;
HWND g_hwnd = nullptr;
ImFont* g_title = nullptr;
ImFont* g_body = nullptr;
ImFont* g_mono = nullptr;

// --- the state the window shows -----------------------------------------------------------------------------------

enum class Page { Welcome, Source, Folder, Install, Done, Failed };
Page g_page = Page::Welcome;
std::wstring g_source;   // the game: a folder or an image
bool g_source_is_image = false;
std::string g_source_note;
bool g_source_ok = false;
std::wstring g_target;
bool g_shortcut_desktop = true, g_shortcut_menu = true;

std::mutex g_lock;
std::string g_status;       // under g_lock
std::vector<std::string> g_log;
std::atomic<double> g_progress{0};
std::atomic<bool> g_working{false}, g_cancel{false};
std::string g_error;

void Log(const std::string& s) {
    std::lock_guard l(g_lock);
    g_log.push_back(s);
    g_status = s;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1)) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Sha1(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t digest[20] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0);
            BCryptFinishHash(h, digest, sizeof(digest), 0);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    std::string hex;
    for (uint8_t b : digest) hex += "0123456789abcdef"[b >> 4], hex += "0123456789abcdef"[b & 15];
    return hex;
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// --- picking -------------------------------------------------------------------------------------------------------

bool Pick(bool folder, std::wstring& out) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return false;
    DWORD opts = 0;
    d->GetOptions(&opts);
    if (folder) {
        d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    } else {
        COMDLG_FILTERSPEC spec[] = {{L"Xbox disc image (*.iso, *.xiso)", L"*.iso;*.xiso"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, spec);
        d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }
    if (FAILED(d->Show(g_hwnd))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(d->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

// the chosen game checked: its default.xbe the release's
void CheckSource() {
    g_source_ok = false;
    std::vector<uint8_t> xbe;
    if (g_source_is_image) {
        xdvdfs::Image img;
        std::string err;
        if (!img.Open(g_source, err)) {
            g_source_note = err;
            return;
        }
        const xdvdfs::Entry* e = img.Find("default.xbe");
        if (!e || !img.Read(*e, xbe)) {
            g_source_note = T("The image has no default.xbe: it is not this game's disc.");
            return;
        }
    } else {
        std::ifstream f(fs::path(g_source) / "default.xbe", std::ios::binary);
        if (!f) {
            g_source_note = T("The folder has no default.xbe: choose the folder that holds the game's files\n(default.xbe, COD.PAK, "
                              "AUDIO, SHELL, SB_AGD, ...).");
            return;
        }
        xbe.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::string sha = Sha1(xbe);
    if (sha != kXbeSha1) {
        g_source_note = std::string(T("This copy is a different release of the game. The port only runs the PAL (Europe) "
                                      "release, version 3.")) +
                        "\n(default.xbe SHA-1 " + sha.substr(0, 12) + "...)";
        return;
    }
    g_source_ok = true;
    g_source_note = T("Your copy is the right release.");
}

// --- installing ----------------------------------------------------------------------------------------------------

struct Payload {
    std::vector<std::pair<uint64_t, std::string>> files;  // size, path inside the install folder
    std::vector<fs::path> parts;
    uint64_t total = 0;
};

bool ReadPayload(Payload& p, std::string& err) {
    fs::path dir = ExeDir();
    std::ifstream idx(dir / "FinestHourRemastered.index");
    if (!idx) {
        err = "FinestHourRemastered.index is missing beside the installer.";
        return false;
    }
    std::string magic;
    int parts = 0;
    idx >> magic >> parts >> p.total;
    if (magic != "FHR1" || parts <= 0) {
        err = "FinestHourRemastered.index is damaged.";
        return false;
    }
    std::string line;
    std::getline(idx, line);
    while (std::getline(idx, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        p.files.push_back({std::stoull(line.substr(0, tab)), line.substr(tab + 1)});
    }
    for (int i = 1; i <= parts; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"FinestHourRemastered.data.%03d", i);
        if (!fs::exists(dir / name)) {
            err = "The payload part " + Narrow(name) + " is missing beside the installer.";
            return false;
        }
        p.parts.push_back(dir / name);
    }
    return true;
}

// the downloads beside the installer: the index and every data part it names, whole (their sizes add up to its
// total; a download cut short does not). What is missing, for the welcome page, one line a file
constexpr wchar_t kReleaseUrl[] = L"https://github.com/crazyriddler/FinestHourRemastered/releases/tag/v1.0";
std::vector<std::string> g_missing;

void CheckDownloads() {
    g_missing.clear();
    fs::path dir = ExeDir();
    std::ifstream idx(dir / "FinestHourRemastered.index");
    std::string magic;
    int parts = 0;
    uint64_t total = 0;
    if (!idx || !(idx >> magic >> parts >> total) || magic != "FHR1" || parts <= 0) {
        g_missing.push_back("FinestHourRemastered.index");
        g_missing.push_back("FinestHourRemastered.data.001, .002, .003 (every part)");
        return;
    }
    uint64_t have = 0;
    for (int i = 1; i <= parts; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"FinestHourRemastered.data.%03d", i);
        std::error_code ec;
        uint64_t size = fs::file_size(dir / name, ec);
        if (ec) g_missing.push_back(Narrow(name));
        else have += size;
    }
    if (g_missing.empty() && have != total)
        g_missing.push_back("a .data part is incomplete (its download did not finish): download the parts again");
}

// a stream over the payload's parts, one after another
struct PartStream {
    std::vector<fs::path> parts;
    size_t i = 0;
    FILE* f = nullptr;
    ~PartStream() {
        if (f) std::fclose(f);
    }
    size_t Read(uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (!f) {
                if (i >= parts.size()) break;
                f = _wfopen(parts[i++].c_str(), L"rb");
                if (!f) break;
            }
            size_t r = std::fread(dst + got, 1, n - got, f);
            got += r;
            if (r == 0) std::fclose(f), f = nullptr;
        }
        return got;
    }
};

void CreateShortcut(const fs::path& lnk, const fs::path& target) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetDescription(kProduct);
    ComPtr<IPersistFile> pf;
    if (SUCCEEDED(sl.As(&pf))) pf->Save(lnk.c_str(), TRUE);
}

// the installation's version (version.txt, written by the installer and the updater; an earlier version has none)
std::string InstalledVersion(const fs::path& dir) {
    std::ifstream f(dir / "version.txt");
    std::string v;
    if (!f || !std::getline(f, v) || v.empty()) return T("an earlier version");
    return v;
}

// the update written into an installation: its files, an earlier version's left overs gone, version.txt. check: the
// base files it leaves alone compared by size first (g_differ: those not as the release's)
// the game running from the folder (its exe of today or the first release's): its files are in use. close: ended
// (a game left running without its window, or the player's: the update was asked for), true while one still runs
bool GameRunningIn(const fs::path& folder, bool close = false) {
    std::error_code ec;
    const fs::path want = fs::weakly_canonical(folder, ec);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, kExe) != 0 && _wcsicmp(pe.szExeFile, L"FinestHourRemastered.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            fs::path dir = fs::weakly_canonical(fs::path(path).parent_path(), ec);
            found = _wcsicmp(dir.c_str(), want.c_str()) == 0;
        }
        CloseHandle(h);
        if (found && close) {
            if (HANDLE k = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
                Log(T("Closing Finest Hour Remastered..."));
                if (TerminateProcess(k, 0) && WaitForSingleObject(k, 10000) == WAIT_OBJECT_0) found = false;
                CloseHandle(k);
            }
        }
    }
    CloseHandle(snap);
    return found;
}

// a file's SHA-1 as the update lists it, empty when it cannot be read
std::string FileSha1(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Sha1(data);
}

bool ApplyPatch(const fs::path& target, bool check, const std::function<bool(uint64_t)>& step, std::string& err) {
    std::error_code ec;
    if (GameRunningIn(target, true)) {
        err = T("Finest Hour Remastered is running from this folder: close it, then try again.");
        return false;
    }
    if (check) {
        Log(T("Checking the installed files..."));
        g_differ = 0;
        for (const auto& [size, rel] : g_patch.checks) {
            uint64_t have = fs::file_size(target / fs::path(Widen(rel)), ec);
            if (ec || have != size) g_differ++;
        }
    }
    for (const PatchFile& pf : g_patch.files) {
        fs::path to = target / fs::path(Widen(pf.path));
        fs::create_directories(to.parent_path(), ec);
        // the same file already there (the runtime's DLL, the documents): left as it is
        if (fs::file_size(to, ec) == pf.size && !ec && FileSha1(to) == pf.sha1) {
            if (!step(pf.size)) {
                err = T("Cancelled.");
                return false;
            }
            continue;
        }
        // written beside, then put in place: a file in use or a full disk leaves the old one whole
        fs::path tmp = to;
        tmp += L".new";
        FILE* o = _wfopen(tmp.c_str(), L"wb");
        if (!o) {
            err = T("Cannot write ") + pf.path;
            return false;
        }
        bool ok = std::fwrite(g_patch.data.data() + pf.at, 1, size_t(pf.size), o) == pf.size;
        ok = std::fclose(o) == 0 && ok;
        // put in place: a read-only file made writable, a file a scanner holds for a moment tried again
        std::error_code rec;
        for (int attempt = 0; ok && attempt < 6; attempt++) {
            if (attempt) SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL), Sleep(500);
            fs::rename(tmp, to, rec);
            if (!rec) break;
        }
        if (ok && rec) ok = false;
        if (!ok) {
            fs::remove(tmp, ec);
            err = T("Cannot write ") + pf.path + (rec ? " (" + rec.message() + ")" : std::string());
            return false;
        }
        if (!step(pf.size)) {
            err = T("Cancelled.");
            return false;
        }
    }
    for (const std::string& rel : g_patch.deletes) fs::remove(target / fs::path(Widen(rel)), ec);
    if (FILE* f = _wfopen((target / "version.txt").c_str(), L"w")) std::fprintf(f, "%s\n", g_patch.version.c_str()), std::fclose(f);
    return true;
}

// the game's language in a new installation: Windows' own when the game has it (codfh.toml's language line)
void SetGameLanguage(const fs::path& target) {
    fs::path toml = target / "codfh.toml";
    std::vector<std::string> lines;
    {
        std::ifstream f(toml);
        for (std::string l; std::getline(f, l);) lines.push_back(l);
    }
    bool set = false;
    for (std::string& l : lines)
        if (l.rfind("language", 0) == 0 && l.find('=') != std::string::npos) l = "language = " + std::to_string(GameLanguage()), set = true;
    if (!set) lines.insert(lines.begin(), "language = " + std::to_string(GameLanguage()));
    std::ofstream o(toml, std::ios::trunc);
    for (const std::string& l : lines) o << l << "\n";
}

// the shortcuts an earlier version made, pointing at the exe of today (the first release's had another name)
void RepairShortcuts(const fs::path& exe) {
    for (const KNOWNFOLDERID& id : {FOLDERID_Desktop, FOLDERID_Programs}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
            fs::path lnk = fs::path(p) / (std::wstring(kProduct) + L".lnk");
            std::error_code ec;
            if (fs::exists(lnk, ec)) CreateShortcut(lnk, exe);
        }
        CoTaskMemFree(p);
    }
}

// an installation of Finest Hour Remastered (of any version): its exe of today or the first release's
bool IsInstallation(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / kExe, ec) || fs::exists(dir / L"FinestHourRemastered.exe", ec);
}

void Update() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto fail = [](const std::string& e) {
        g_error = e;
        Log("Failed: " + e);
        g_working = false;
        g_page = Page::Failed;
    };
    if (!g_patch.ok) return fail(T("This program has no update inside: download it again."));
    fs::path target = g_target;
    if (!IsInstallation(target)) return fail(T("This folder has no Finest Hour Remastered installation."));
    Log(T("Updating Finest Hour Remastered..."));
    uint64_t total = 0, done = 0;
    for (const PatchFile& pf : g_patch.files) total += pf.size;
    auto step = [&](uint64_t n) {
        done += n;
        g_progress = total ? double(done) / double(total) : 1;
        return !g_cancel.load();
    };
    std::string err;
    if (!ApplyPatch(target, true, step, err)) return fail(err);
    RepairShortcuts(target / kExe);
    g_progress = 1;
    Log(T("Done."));
    g_working = false;
    g_page = Page::Done;
}

void Install() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto fail = [](const std::string& e) {
        g_error = e;
        Log("Failed: " + e);
        g_working = false;
        g_page = Page::Failed;
    };
    Payload pay;
    std::string err;
    if (!ReadPayload(pay, err)) return fail(err);
    fs::path target = g_target, assets = target / "assets";
    std::error_code ec;
    fs::create_directories(assets, ec);
    if (ec) return fail(T("The folder cannot be made: ") + ec.message());
    // the game's own bytes
    uint64_t game_total = 0;
    xdvdfs::Image img;
    std::vector<fs::path> game_files;
    if (g_source_is_image) {
        if (!img.Open(g_source, err)) return fail(err);
        game_total = img.TotalSize();
    } else {
        for (auto& e : fs::recursive_directory_iterator(g_source, ec))
            if (e.is_regular_file(ec)) game_files.push_back(e.path()), game_total += e.file_size(ec);
    }
    uint64_t patch_total = 0;
    for (const PatchFile& pf : g_patch.files) patch_total += pf.size;
    const double all = double(game_total + pay.total + patch_total);
    uint64_t done = 0;
    auto step = [&](uint64_t n) {
        done += n;
        g_progress = all > 0 ? double(done) / all : 1;
        return !g_cancel.load();
    };
    Log(T("Copying the game's files..."));
    std::vector<uint8_t> buf(8 << 20);
    if (g_source_is_image) {
        for (const xdvdfs::Entry& e : img.Files()) {
            fs::path to = assets / fs::path(Widen(e.path));
            fs::create_directories(to.parent_path(), ec);
            if (!img.Extract(e, to.wstring(), step)) return fail(g_cancel ? std::string(T("Cancelled.")) : "Cannot extract " + e.path);
        }
    } else {
        for (const fs::path& from : game_files) {
            fs::path rel = fs::relative(from, g_source, ec);
            fs::path to = assets / rel;
            fs::create_directories(to.parent_path(), ec);
            FILE* i = _wfopen(from.c_str(), L"rb");
            FILE* o = _wfopen(to.c_str(), L"wb");
            bool ok = i && o;
            while (ok) {
                size_t n = std::fread(buf.data(), 1, buf.size(), i);
                if (n == 0) break;
                ok = std::fwrite(buf.data(), 1, n, o) == n && step(n);
            }
            if (i) std::fclose(i);
            if (o) std::fclose(o);
            if (!ok) return fail(g_cancel ? std::string(T("Cancelled.")) : "Cannot copy " + Narrow(rel.wstring()));
        }
    }
    Log(T("Installing Finest Hour Remastered..."));
    PartStream ps{pay.parts};
    for (auto& [size, rel] : pay.files) {
        fs::path to = target / fs::path(Widen(rel));
        fs::create_directories(to.parent_path(), ec);
        FILE* o = _wfopen(to.c_str(), L"wb");
        if (!o) return fail(T("Cannot write ") + rel);
        uint64_t left = size;
        while (left > 0) {
            size_t n = ps.Read(buf.data(), size_t(std::min<uint64_t>(left, buf.size())));
            if (n == 0 || std::fwrite(buf.data(), 1, n, o) != n || !step(n)) {
                std::fclose(o);
                return fail(g_cancel ? std::string(T("Cancelled.")) : "The payload ended early (" + rel + ").");
            }
            left -= n;
        }
        std::fclose(o);
    }
    // the latest update over the base, and the game in Windows' language when it has it
    if (g_patch.ok) {
        Log(T("Applying the latest update..."));
        std::string perr;
        if (!ApplyPatch(target, false, step, perr)) return fail(perr);
    }
    SetGameLanguage(target);
    // the first release's exe, under another name (the game could not start itself again under it)
    fs::remove(target / L"FinestHourRemastered.exe", ec);
    Log(T("Making shortcuts..."));
    fs::path exe = target / kExe;
    PWSTR p = nullptr;
    if (g_shortcut_desktop && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)))
        CreateShortcut(fs::path(p) / (std::wstring(kProduct) + L".lnk"), exe);
    CoTaskMemFree(p), p = nullptr;
    if (g_shortcut_menu && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p)))
        CreateShortcut(fs::path(p) / (std::wstring(kProduct) + L".lnk"), exe);
    CoTaskMemFree(p);
    g_progress = 1;
    Log(T("Done."));
    g_working = false;
    g_page = Page::Done;
}

// --- drawing -------------------------------------------------------------------------------------------------------

const ImVec4 kGold(0.86f, 0.70f, 0.38f, 1.0f), kText(0.92f, 0.90f, 0.85f, 1.0f), kDim(0.62f, 0.60f, 0.55f, 1.0f),
    kRed(0.90f, 0.35f, 0.30f, 1.0f), kGreen(0.55f, 0.82f, 0.45f, 1.0f);

bool Button(const char* label, bool enabled = true, float w = 170) {
    if (!enabled) ImGui::BeginDisabled();
    bool r = ImGui::Button(label, ImVec2(w, 42));
    if (!enabled) ImGui::EndDisabled();
    return r;
}

void Frame() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImVec2 size = vp->Size;
    // the art, covering the window, darkened towards the left where the text is
    if (g_art) {
        float k = std::max(size.x / g_art_w, size.y / g_art_h);
        ImVec2 sz(g_art_w * k, g_art_h * k), at((size.x - sz.x) * 0.5f, (size.y - sz.y) * 0.5f);
        bg->AddImage(reinterpret_cast<ImTextureID>(g_art.Get()), at, ImVec2(at.x + sz.x, at.y + sz.y));
    }
    bg->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(8, 9, 8, 245), IM_COL32(8, 9, 8, 120), IM_COL32(8, 9, 8, 120),
                                IM_COL32(8, 9, 8, 245));
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, 4), IM_COL32(220, 178, 96, 255));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    // the close button, top right
    ImGui::SetCursorPos(ImVec2(size.x - 54, 14));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("X", ImVec2(40, 32)) && !g_working) PostQuitMessage(0);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(56, 48));
    ImGui::PushFont(g_title);
    ImGui::TextColored(kGold, "FINEST HOUR");
    ImGui::SetCursorPosX(56);
    ImGui::TextColored(kText, "REMASTERED");
    ImGui::PopFont();
    ImGui::SetCursorPosX(58);
    ImGui::TextColored(kDim, "%s", T("A native PC port of Call of Duty: Finest Hour  -  by crazyriddler"));
    ImGui::Dummy(ImVec2(0, 28));

    const float x = 58, wrap = 600;
    ImGui::PushTextWrapPos(x + wrap);
    auto para = [&](const char* s, ImVec4 c = kText) {
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(c, "%s", s);
    };
    auto start = [](void (*work)()) {
        g_page = Page::Install;
        g_working = true;
        g_cancel = false;
        std::thread(work).detach();
    };
    switch (g_page) {
        case Page::Welcome:
            if (g_update_mode) {
                para(T("This updates an installed Finest Hour Remastered to the latest version, whatever version it has "
                       "now. Your settings and saved games are kept."));
                ImGui::Dummy(ImVec2(0, 10));
                para((std::string(T("Update:")) + " " + (g_patch.ok ? g_patch.version : std::string("-"))).c_str(), kGold);
                if (!g_patch.ok) para(T("This program has no update inside: download it again."), kRed);
                ImGui::Dummy(ImVec2(0, 14));
                ImGui::SetCursorPosX(x);
                if (Button(T("Continue"), g_patch.ok)) g_page = Page::Folder;
                break;
            }
            if (!g_missing.empty()) {
                // the release's own downloads are needed beside the installer first
                para(T("Before installing, the release's other downloads are needed in this installer's folder:"), kGold);
                para(Narrow(ExeDir().wstring()).c_str(), kDim);
                ImGui::Dummy(ImVec2(0, 6));
                para(T("Missing:"), kRed);
                for (const std::string& m : g_missing) para(("  - " + m).c_str(), kRed);
                ImGui::Dummy(ImVec2(0, 6));
                para(T("Download them from the release page into that folder, then check again."), kDim);
                ImGui::Dummy(ImVec2(0, 12));
                ImGui::SetCursorPosX(x);
                if (Button(T("Release page"), true, 220)) ShellExecuteW(nullptr, L"open", kReleaseUrl, nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::SameLine();
                if (Button(T("Check again"), true, 220)) CheckDownloads();
                break;
            }
            para(T("Welcome. This installs Finest Hour Remastered, a native Windows port of the original Xbox game with a "
                   "remastered renderer, from your own copy of the game."));
            ImGui::Dummy(ImVec2(0, 10));
            para(T("You need:"), kGold);
            para(T(kRequired));
            ImGui::Dummy(ImVec2(0, 10));
            para(T("Its files (a folder with default.xbe), a disc image (.iso) or an XISO. No game files are included: they "
                   "are read from your copy."),
                 kDim);
            ImGui::Dummy(ImVec2(0, 14));
            ImGui::SetCursorPosX(x);
            if (Button(T("Continue"))) g_page = Page::Source;
            break;
        case Page::Source:
            para(T("Where is your copy of the game?"), kGold);
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::SetCursorPosX(x);
            if (Button(T("Game folder..."), true, 230)) {
                std::wstring p;
                if (Pick(true, p)) g_source = p, g_source_is_image = false, CheckSource();
            }
            ImGui::SameLine();
            if (Button(T("Disc image / XISO..."), true, 260)) {
                std::wstring p;
                if (Pick(false, p)) g_source = p, g_source_is_image = true, CheckSource();
            }
            ImGui::Dummy(ImVec2(0, 14));
            if (!g_source.empty()) {
                para(Narrow(g_source).c_str(), kDim);
                para(g_source_note.c_str(), g_source_ok ? kGreen : kRed);
            }
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button(T("Back"))) g_page = Page::Welcome;
            ImGui::SameLine();
            if (Button(T("Continue"), g_source_ok)) g_page = Page::Folder;
            break;
        case Page::Folder: {
            para(g_update_mode ? T("Where is Finest Hour Remastered installed?") : T("Install to:"), kGold);
            ImGui::Dummy(ImVec2(0, 6));
            static char buf[1024] = {};
            if (!buf[0]) std::snprintf(buf, sizeof(buf), "%s", Narrow(g_target).c_str());
            ImGui::SetCursorPosX(x);
            ImGui::SetNextItemWidth(470);
            if (ImGui::InputText("##target", buf, sizeof(buf))) g_target = Widen(buf);
            ImGui::SameLine();
            if (Button(T("Browse..."), true, 150)) {
                std::wstring p;
                if (Pick(true, p)) {
                    // the updater takes the installation's folder itself, the installer a new one inside the chosen
                    g_target = g_update_mode ? p : (fs::path(p) / kProduct).wstring();
                    std::snprintf(buf, sizeof(buf), "%s", Narrow(g_target).c_str());
                }
            }
            ImGui::Dummy(ImVec2(0, 6));
            bool can = !g_target.empty();
            if (g_update_mode) {
                can = can && IsInstallation(g_target);
                if (!g_target.empty() && !can) para(T("This folder has no Finest Hour Remastered installation."), kRed);
                if (can) para((std::string(T("Installed:")) + " " + InstalledVersion(g_target)).c_str(), kDim);
                para((std::string(T("Update:")) + " " + g_patch.version).c_str(), kGold);
            } else {
                para(T("About 9 GB: the game's files and the remastered textures."), kDim);
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::SetCursorPosX(x);
                ImGui::Checkbox(T("Desktop shortcut"), &g_shortcut_desktop);
                ImGui::SetCursorPosX(x);
                ImGui::Checkbox(T("Start menu shortcut"), &g_shortcut_menu);
            }
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button(T("Back"))) g_page = g_update_mode ? Page::Welcome : Page::Source;
            ImGui::SameLine();
            if (Button(g_update_mode ? T("Update") : T("Install"), can)) start(g_update_mode ? Update : Install);
            break;
        }
        case Page::Install: {
            std::string status;
            {
                std::lock_guard l(g_lock);
                status = g_status;
            }
            para(status.c_str(), kGold);
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::SetCursorPosX(x);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kGold);
            char pct[32];
            std::snprintf(pct, sizeof(pct), "%.0f%%", g_progress.load() * 100);
            ImGui::ProgressBar(float(g_progress.load()), ImVec2(wrap, 26), pct);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button(T("Cancel"), !g_cancel)) g_cancel = true;
            break;
        }
        case Page::Done:
            para(g_update_mode ? T("Finest Hour Remastered is up to date.") : T("Finest Hour Remastered is installed."), kGreen);
            ImGui::Dummy(ImVec2(0, 8));
            if (g_update_mode && g_differ)
                para(T("Some installed files differ from the release and are not in this update. If the game misbehaves, "
                       "install it again with the full release."),
                     kRed);
            else
                para(T("In the game, choose PC Settings in the main menu (or press F10) for the display, graphics and control "
                       "options. The first start prepares the shaders for your graphics card."),
                     kDim);
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button(T("Play now"))) {
                fs::path exe = fs::path(g_target) / kExe;
                ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(), SW_SHOWNORMAL);
                PostQuitMessage(0);
            }
            ImGui::SameLine();
            if (Button(T("Close"))) PostQuitMessage(0);
            break;
        case Page::Failed:
            para(g_update_mode ? T("The update did not finish.") : T("The installation did not finish."), kRed);
            para(g_error.c_str(), kDim);
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button(T("Back"))) g_page = Page::Folder;
            ImGui::SameLine();
            if (Button(T("Close"))) PostQuitMessage(0);
            break;
    }
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(ImVec2(58, size.y - 40));
    ImGui::TextColored(kDim, "%s", T("Not affiliated with Activision. Call of Duty is a trademark of Activision."));
    ImGui::End();
}

// --- the window and Direct3D 11 ------------------------------------------------------------------------------------

bool LoadArt() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_ART), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    const void* data = LockResource(g);
    DWORD size = SizeofResource(nullptr, r);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), size);
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    dec->GetFrame(0, &frame);
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w, h;
    conv->GetSize(&w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data());
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex))) return false;
    g_dev->CreateShaderResourceView(tex.Get(), nullptr, &g_art);
    g_art_w = int(w), g_art_h = int(h);
    return true;
}

ImFont* FontFromResource(int id, float px) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return nullptr;
    void* data = LockResource(LoadResource(nullptr, r));
    DWORD size = SizeofResource(nullptr, r);
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(size), px, &cfg);
}

void MakeTarget() {
    g_rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_swap && w != SIZE_MINIMIZED) {
                g_rtv.Reset();
                g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
                MakeTarget();
            }
            return 0;
        case WM_NCHITTEST: {
            // the top band drags the window (no title bar), the close button aside
            LRESULT hit = DefWindowProcW(h, msg, w, l);
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &p);
            RECT rc;
            GetClientRect(h, &rc);
            if (hit == HTCLIENT && p.y < 44 && p.x < rc.right - 64) return HTCAPTION;
            return hit;
        }
        case WM_CLOSE:
            if (g_working) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PWSTR pf = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &pf)))
        g_target = (fs::path(pf).parent_path() / L"Games" / kProduct).wstring();
    CoTaskMemFree(pf);
    if (g_target.empty()) g_target = L"C:\\Games\\Finest Hour Remastered";
    g_lang = SystemLanguage();
    ReadPatch(g_patch);
    {
        // the updater: this exe under the update's name
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring name = fs::path(self).filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        g_update_mode = name.find(L"update") != std::wstring::npos;
    }
    CheckDownloads();

    // unattended: --source <the game's folder or disc image> [--target <folder>] [--shortcuts] [--log <file>]; the
    // same checks and steps as the window's, the outcome in the exit code (0 installed, 1 failed, 2 not the release)
    // and the log
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); argv && argc > 2) {
        std::wstring log;
        bool unattended = false;
        g_shortcut_desktop = g_shortcut_menu = false;
        bool update = false;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--update" && i + 1 < argc) g_target = argv[++i], update = true;
            else if (a == L"--source" && i + 1 < argc) g_source = argv[++i], unattended = true;
            else if (a == L"--target" && i + 1 < argc) g_target = argv[++i];
            else if (a == L"--log" && i + 1 < argc) log = argv[++i];
            else if (a == L"--shortcuts") g_shortcut_desktop = g_shortcut_menu = true;
        }
        LocalFree(argv);
        // unattended update: --update <the installation's folder> [--log <file>] (0 updated, 1 failed)
        if (update) {
            g_update_mode = true;
            Update();
            int code = g_page == Page::Done ? 0 : 1;
            if (!log.empty())
                if (FILE* f = _wfopen(log.c_str(), L"w")) {
                    std::fprintf(f, "update %s to %s\ndiffering files %zu\n", Narrow(g_target).c_str(), g_patch.version.c_str(), g_differ);
                    for (const std::string& l : g_log) std::fprintf(f, "%s\n", l.c_str());
                    std::fprintf(f, "exit %d\n", code);
                    std::fclose(f);
                }
            return code;
        }
        if (unattended) {
            g_source_is_image = fs::is_regular_file(g_source);
            CheckSource();
            int code = 2;
            if (g_source_ok) {
                Install();
                code = g_page == Page::Done ? 0 : 1;
            }
            if (!log.empty())
                if (FILE* f = _wfopen(log.c_str(), L"w")) {
                    std::fprintf(f, "source: %s\ncheck: %s\n", Narrow(g_source).c_str(), g_source_note.c_str());
                    for (const std::string& l : g_log) std::fprintf(f, "%s\n", l.c_str());
                    std::fprintf(f, "exit %d\n", code);
                    std::fclose(f);
                }
            return code;
        }
        g_shortcut_desktop = g_shortcut_menu = true;
    }

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst};
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FinestHourSetup";
    RegisterClassExW(&wc);
    float dpi = GetDpiForSystem() / 96.0f;
    int ww = int(1040 * dpi), wh = int(640 * dpi);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, g_update_mode ? L"Finest Hour Remastered Update" : L"Finest Hour Remastered Setup", WS_POPUP | WS_MINIMIZEBOX,
                             sx, sy, ww, wh, nullptr, nullptr, inst, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &sd, &g_swap, &g_dev, nullptr, &g_ctx)) &&
        FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                             &g_swap, &g_dev, nullptr, &g_ctx))) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", kProduct, MB_ICONERROR);
        return 1;
    }
    MakeTarget();
    ShowWindow(g_hwnd, show);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    g_body = FontFromResource(IDR_FONT_BODY, 18 * dpi);
    g_title = FontFromResource(IDR_FONT_TITLE, 58 * dpi);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    if (!g_title) g_title = g_body;
    io.FontDefault = g_body;
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 3, st.FramePadding = ImVec2(12, 8), st.ItemSpacing = ImVec2(12, 10);
    st.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.19f, 0.15f, 0.92f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.43f, 0.22f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.58f, 0.28f, 1.0f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.10f, 0.92f);
    st.Colors[ImGuiCol_CheckMark] = kGold;
    st.Colors[ImGuiCol_Text] = kText;
    st.ScaleAllSizes(dpi);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());
    LoadArt();
    if (const wchar_t* pg = _wgetenv(L"FHR_SETUP_PAGE")) {
        g_page = Page(std::clamp(_wtoi(pg), 0, 5));
        if (g_page == Page::Source) g_source = L"D:\\Games\\COD", g_source_ok = true, g_source_note = "Your copy is the right release.";
        if (g_page == Page::Install) Log("Installing Finest Hour Remastered..."), g_progress = 0.42;
    }

    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto out;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        const float clear[4] = {0.03f, 0.03f, 0.03f, 1};
        g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // FHR_SETUP_SHOT=<file.bmp> (FHR_SETUP_PAGE=0..5): the window's picture after a few frames, then quit (docs)
        static int frames = 0;
        if (const wchar_t* shot = _wgetenv(L"FHR_SETUP_SHOT"); shot && ++frames == 20) {
            ComPtr<ID3D11Texture2D> bb, st;
            g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
            D3D11_TEXTURE2D_DESC d;
            bb->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
            g_dev->CreateTexture2D(&d, nullptr, &st);
            g_ctx->CopyResource(st.Get(), bb.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                FILE* f = _wfopen(shot, L"wb");
                if (f) {
                    uint32_t row = d.Width * 3 + 3 & ~3u, size = 54 + row * d.Height;
                    uint8_t hdr[54] = {'B', 'M'};
                    auto put = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
                    put(2, size), put(10, 54), put(14, 40), put(18, d.Width), put(22, d.Height), put(34, row * d.Height);
                    hdr[26] = 1, hdr[28] = 24;
                    std::fwrite(hdr, 1, 54, f);
                    std::vector<uint8_t> line(row, 0);
                    for (uint32_t y = d.Height; y-- > 0;) {
                        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                        for (uint32_t x = 0; x < d.Width; x++) line[x * 3] = s[x * 4 + 2], line[x * 3 + 1] = s[x * 4 + 1], line[x * 3 + 2] = s[x * 4];
                        std::fwrite(line.data(), 1, row, f);
                    }
                    std::fclose(f);
                }
                g_ctx->Unmap(st.Get(), 0);
            }
            break;
        }
        g_swap->Present(1, 0);
    }
out:
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
