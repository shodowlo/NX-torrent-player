#include "i18n.hpp"

#include <unordered_map>

#include "config.hpp"

namespace
{

struct Entry
{
    const char* en;
    const char* fr;
};

// Keyed on the English string as it is written at the call site. A string with
// no row here shows in English, which is why brand names (Stremio, Magnet), the
// ZR diagnostic panel and anything already identical in French are absent
// rather than repeated.
//
// Trailing spaces are part of the key: several of these are concatenated with a
// value after them, and dropping the space in the French would run the two
// together.
const Entry kFr[] = {
    // ---- browse: addons, sources, episodes --------------------------------
    { "Addons unavailable: ", "Addons indisponibles : " },
    { "Addons unavailable", "Addons indisponibles" },
    { "No addon provides a source for this title.",
      "Aucun addon ne propose de source pour ce titre." },
    { "No addon provides a source for this title",
      "Aucun addon ne propose de source pour ce titre" },
    { "Pick an addon", "Choisir un addon" },
    { "Sources unavailable: ", "Sources indisponibles : " },
    { "Sources unavailable", "Sources indisponibles" },
    { "This addon has no source for this title.",
      "Cet addon n'a aucune source pour ce titre." },
    { "This addon only offers 4K sources, which are hidden in Options.",
      "Cet addon ne propose que des sources 4K, masquées dans les Options." },
    { "No source from this addon", "Aucune source depuis cet addon" },
    { "Pick a source", "Choisir une source" },
    { "Loading sources...", "Chargement des sources..." },
    { "Unsupported source: only torrents (infoHash) can be played for now.",
      "Source non prise en charge : seuls les torrents (infoHash) sont "
      "lisibles pour l'instant." },
    { "Episode ", "Épisode " },
    { "Episode", "Épisode" },
    { " - Season ", " - Saison " },
    { "Season ", "Saison " },
    { "Pick an episode", "Choisir un épisode" },
    { "Previous episode", "Épisode précédent" },
    { "Next episode", "Épisode suivant" },
    { "Next episode...", "Épisode suivant..." },
    { "Could not open that episode.", "Impossible d'ouvrir cet épisode." },
    { "No episodes found", "Aucun épisode trouvé" },
    { "Episodes unavailable", "Épisodes indisponibles" },
    { "No episodes", "Aucun épisode" },
    { "Specials", "Hors-série" },
    { "Remove from library", "Retirer de la bibliothèque" },
    { "Add to library", "Ajouter à la bibliothèque" },
    { "Could not add it to your library.",
      "Impossible de l'ajouter à votre bibliothèque." },
    { "Could not remove it from your library.",
      "Impossible de le retirer de votre bibliothèque." },
    { "Loading...", "Chargement..." },
    { "Show", "Série" },

    // ---- the Local tab ----------------------------------------------------
    { "This torrent has no video file to stream.",
      "Ce torrent ne contient aucun fichier vidéo à lire." },
    { "Pick a file", "Choisir un fichier" },
    { "Still fetching this magnet's files -- please wait.",
      "Récupération des fichiers de ce magnet en cours -- patientez." },
    { "  (waiting)", "  (en attente)" },
    { "Magnet link or info hash", "Lien magnet ou info-hash" },
    { "magnet:?xt=... or a 40-character hash",
      "magnet:?xt=... ou un hash de 40 caractères" },
    { "That does not look like a magnet link or an info hash.",
      "Cela ne ressemble ni à un lien magnet ni à un info-hash." },
    { "No torrents found", "Aucun torrent trouvé" },
    { "Drop .torrent files in this folder on your SD card, or add magnet "
      "links to magnet.txt inside it (one per line):",
      "Déposez des fichiers .torrent dans ce dossier de la carte SD, ou "
      "ajoutez des liens magnet dans magnet.txt (un par ligne) :" },
    { "Delete", "Supprimer" },
    { "Remove this magnet from the list?", "Retirer ce magnet de la liste ?" },
    { "Delete this .torrent file from the SD card?",
      "Supprimer ce fichier .torrent de la carte SD ?" },
    { "Cancel", "Annuler" },
    { "+  Add magnet", "+  Ajouter un magnet" },
    { "Back", "Retour" },
    { "View", "Vue" },

    // ---- the player -------------------------------------------------------
    { "Fetching metadata...", "Récupération des métadonnées..." },
    { "Opening torrent...", "Ouverture du torrent..." },
    { "Failed: ", "Échec : " },
    { "Player initialisation failed", "Échec de l'initialisation du lecteur" },
    { "Connecting to peers...", "Connexion aux peers..." },
    { "Downloading header...", "Téléchargement de l'en-tête..." },
    { "Buffering...", "Mise en mémoire tampon..." },
    { "Press B to go back", "Appuyez sur B pour revenir" },
    { "Lock", "Verrouiller" },
    { "Seek -", "Reculer" },
    { "Seek +", "Avancer" },
    { "Slower", "Plus lent" },
    { "Faster", "Plus rapide" },
    { "Info", "Infos" },
    { "Up next", "À suivre" },
    { "Close", "Fermer" },
    { "Playback", "Lecture" },
    { "Track ", "Piste " },
    { "Track", "Piste" },
    { "Speed", "Vitesse" },
    { "Speed  ", "Vitesse  " },
    { "Off", "Désactivé" },
    { "None", "Aucun" },
    { "Unknown", "Inconnu" },
    { "SUBTITLES", "SOUS-TITRES" },
    { "Subtitles", "Sous-titres" },
    { "Subtitles  ", "Sous-titres  " },
    { "Subtitle", "Sous-titre" },
    { "Subtitle size", "Taille des sous-titres" },
    { "Subtitle delay", "Décalage des sous-titres" },
    { "Subtitles earlier", "Sous-titres plus tôt" },
    { "Subtitles later", "Sous-titres plus tard" },
    { "L / R shift the subtitles by 0.1 s. Later is positive. The video keeps "
      "playing behind this.",
      "L / R décalent les sous-titres de 0,1 s. Plus tard = positif. La vidéo "
      "continue de jouer derrière." },
    { "Searching addons...", "Recherche dans les addons..." },
    { "Downloading ", "Téléchargement de " },
    { " loaded", " chargé" },
    { "Subtitle download failed", "Échec du téléchargement du sous-titre" },
    { "Subtitle could not be loaded", "Le sous-titre n'a pas pu être chargé" },

    // ---- Options ----------------------------------------------------------
    { " (default)", " (par défaut)" },
    { "General", "Général" },
    { "About", "À propos" },
    { "Category on startup", "Catégorie au démarrage" },
    { "Language", "Langue" },
    { "The language applies when you restart the app.",
      "La langue s'applique au redémarrage de l'application." },
    { "Theme", "Thème" },
    { "Dark (default)", "Sombre (par défaut)" },
    { "Light", "Clair" },
    { "Follow the console", "Suivre la console" },
    { "The theme applies when you restart the app.",
      "Le thème s'applique au redémarrage de l'application." },
    { "Accent colour", "Couleur d'accent" },
    { "List style", "Style de liste" },
    { "Posters (default)", "Affiches (par défaut)" },
    { "Cards", "Cartes" },
    { "Classic", "Classique" },
    { "UI size", "Taille de l'interface" },
    { "Docked", "Sur le dock" },
    { "Handheld", "En portable" },
    { "Audio language", "Langue audio" },
    { "Subtitle language", "Langue des sous-titres" },
    { "Boost quiet audio in handheld", "Amplifier l'audio faible en portable" },
    { "On by default. Lifts a 5.1 mix folded down to stereo, which plays much "
      "quieter.",
      "Activé par défaut. Relève un mixage 5.1 replié en stéréo, bien plus "
      "faible." },
    { "Hardware decoding", "Décodage matériel" },
    { "On by default. Off decodes in software: slower, may stutter on 1080p.",
      "Activé par défaut. Désactivé, le décodage est logiciel : plus lent, "
      "saccades possibles en 1080p." },
    { "Stream to RAM (no SD cache)", "Streaming en RAM (sans cache SD)" },
    { "Off (Not recommended)", "Désactivé (déconseillé)" },
    { "On (Recommended)", "Activé (recommandé)" },
    { "Keep pieces in memory instead of writing them to the SD card. Removes "
      "the stutter on each finished piece, at the cost of no resume and a "
      "limited seek-back range.",
      "Garde les pièces en mémoire au lieu de les écrire sur la carte SD. "
      "Supprime la saccade à chaque pièce terminée, au prix de l'absence de "
      "reprise et d'un retour arrière limité." },
    { "Limit download rate", "Limiter le débit de téléchargement" },
    { "Off (default)", "Désactivé (par défaut)" },
    { "On", "Activé" },
    { "Once the buffer is comfortably ahead, cap the download speed instead "
      "of bursting \xE2\x80\x94 the bursts can stutter the system. Off by default.",
      "Une fois le tampon confortablement en avance, plafonne la vitesse au "
      "lieu de saturer \xE2\x80\x94 les pics peuvent faire saccader le système. Désactivé "
      "par défaut." },
    { "Hide 4K sources", "Masquer les sources 4K" },
    { "Poster cache", "Cache des affiches" },
    { "Reading...", "Lecture..." },
    { " on the SD card", " sur la carte SD" },
    { "Clear poster cache", "Vider le cache des affiches" },
    { "Clearing...", "Vidage..." },
    { "Poster cache cleared. The library reloads when you go back to it.",
      "Cache des affiches vidé. La bibliothèque se recharge à votre retour." },
    { "Diagnostics", "Diagnostic" },
    { "Log file", "Fichier de log" },
    { "The log is written to the SD card continuously. Turn it on to diagnose "
      "a problem, then restart the app.",
      "Le log est écrit en continu sur la carte SD. Activez-le pour "
      "diagnostiquer un problème, puis redémarrez l'application." },

    // ---- the account screen -----------------------------------------------
    { "Account", "Compte" },
    { "Not signed in", "Non connecté" },
    { "The sign-in form is on the Stremio tab.",
      "Le formulaire de connexion est dans l'onglet Stremio." },
    { "this console", "cette console" },
    { "Signed in to Stremio", "Connecté à Stremio" },
    { "In library", "En bibliothèque" },
    { "Installed addons", "Addons installés" },
    { "Reading the account's addons...", "Lecture des addons du compte..." },
    { "Could not read them: ", "Impossible de les lire : " },
    { "Metadata", "Métadonnées" },
    { "Streams (disabled)", "Flux (désactivés)" },
    { "Streams", "Flux" },
    { "Nothing this app uses", "Rien que cette application utilise" },
    { "None. Install them from Stremio on another device.",
      "Aucun. Installez-les depuis Stremio sur un autre appareil." },
    { "Sign out of Stremio", "Se déconnecter de Stremio" },
    { "Signed out. Restart the app to get back to the sign-in screen.",
      "Déconnecté. Redémarrez l'application pour revenir à l'écran de "
      "connexion." },

    // ---- the Stremio tab --------------------------------------------------
    { "Continue", "Reprendre" },
    { "Continue Watching", "Reprendre le visionnage" },
    { "Continue watching", "Reprendre le visionnage" },
    { "Movies", "Films" },
    { "Movie", "Film" },
    { "Shows", "Séries" },
    { "Library", "Bibliothèque" },
    { "Search", "Recherche" },
    { "Popular", "Populaire" },
    { "Popular Movies", "Films populaires" },
    { "Popular Shows", "Séries populaires" },
    { "No popular movies", "Aucun film populaire" },
    { "No popular shows", "Aucune série populaire" },
    { "Nothing in progress", "Rien en cours" },
    { " items", " éléments" },
    { "Library is empty", "Bibliothèque vide" },
    { "Loading library...", "Chargement de la bibliothèque..." },
    { "Library unavailable: ", "Bibliothèque indisponible : " },
    { "This catalogue is unavailable: ", "Ce catalogue est indisponible : " },
    { "Catalog unavailable", "Catalogue indisponible" },
    { "Nothing here", "Rien ici" },
    { "Error", "Erreur" },
    { "Search movies & shows...", "Rechercher films et séries..." },
    { "Searching...", "Recherche..." },
    { "No results for \"", "Aucun résultat pour \"" },
    { "Search Stremio", "Rechercher sur Stremio" },
    { "See More", "Voir plus" },
    { "Featured", "À la une" },
    { "Year", "Année" },
    { "All", "Tout" },
    { " – present", " – aujourd'hui" },
    { "Remove", "Retirer" },
    { "Could not remove it", "Impossible de le retirer" },
    { "Reload", "Recharger" },

    // ---- signing in -------------------------------------------------------
    { "Sign in to Stremio", "Se connecter à Stremio" },
    { "Your library, your addons and their sources.",
      "Votre bibliothèque, vos addons et leurs sources." },
    { "EMAIL", "E-MAIL" },
    { "PASSWORD", "MOT DE PASSE" },
    { "Not entered", "Non renseigné" },
    { "Sign in", "Se connecter" },
    { "Signing in...", "Connexion..." },
    { "Signed in", "Connecté" },
    { "Sign-in failed: ", "Échec de la connexion : " },
    { "Stremio email", "E-mail Stremio" },
    { "Stremio password", "Mot de passe Stremio" },
    { "Enter an email and a password.",
      "Saisissez un e-mail et un mot de passe." },
    { "Wrong email or password", "E-mail ou mot de passe incorrect" },
    { "Sign in with a QR code", "Se connecter avec un QR code" },
    { "Scan with a phone signed in to Stremio",
      "Scannez avec un téléphone connecté à Stremio" },
    { "Or open link.stremio.com and enter this code:",
      "Ou ouvrez link.stremio.com et saisissez ce code :" },
    { "Getting a code...", "Obtention d'un code..." },
    { "Waiting for your phone...", "En attente du téléphone..." },
    { "Could not get a code: ", "Impossible d'obtenir un code : " },
    { "unexpected answer", "réponse inattendue" },
    { "Unexpected answer from the addon", "Réponse inattendue de l'addon" },
    { "Not a Stremio playback", "Lecture hors Stremio" },

    // ---- updates ----------------------------------------------------------
    { "Check for updates on startup",
      "Vérifier les mises à jour au démarrage" },
    { "View changelog", "Voir le journal des modifications" },
    { "Check now", "Vérifier maintenant" },
    { "Checking...", "Vérification..." },
    { "Could not check for updates: ",
      "Impossible de vérifier les mises à jour : " },
    { "You are on the latest version (",
      "Vous êtes à la dernière version (" },
    { " is out, but that release has no .nro to install. Get it from GitHub.",
      " est disponible, mais cette version ne fournit aucun .nro à installer. "
      "Récupérez-la sur GitHub." },
    { " \xE2\x80\x94 an update is installed, restart to use it",
      " \xE2\x80\x94 une mise à jour est installée, redémarrez pour l'utiliser" },
    { " is available.", " est disponible." },
    { "Later", "Plus tard" },
    { "Update", "Mettre à jour" },
    { "Downloading...", "Téléchargement..." },
    { "Downloading... ", "Téléchargement... " },
    { "Downloading... 0%", "Téléchargement... 0%" },
    { "Update failed: ", "Échec de la mise à jour : " },
    { "Update installed. The app will restart to use it.",
      "Mise à jour installée. L'application va redémarrer pour l'utiliser." },
    { "Restart", "Redémarrer" },
    { "Close and reopen the app to use the new version.",
      "Fermez et rouvrez l'application pour utiliser la nouvelle version." },
    { "Changelog \xE2\x80\x94 ", "Journal des modifications \xE2\x80\x94 " },
    { "Loading changelog...", "Chargement du journal..." },
    { "Could not load the changelog: ",
      "Impossible de charger le journal : " },
    { "unknown install path", "chemin d'installation inconnu" },
    { "no release found", "aucune version trouvée" },
    { "no changelog for this version",
      "aucun journal des modifications pour cette version" },
    { "nothing to download", "rien à télécharger" },

    // ---- the language table in config.cpp ---------------------------------
    { "Console language", "Langue de la console" },
    { "English", "Anglais" },
    { "French", "Français" },
    { "Spanish", "Espagnol" },
    { "German", "Allemand" },
    { "Italian", "Italien" },
    { "Portuguese", "Portugais" },
    { "Dutch", "Néerlandais" },
    { "Russian", "Russe" },
    { "Japanese", "Japonais" },
    { "Korean", "Coréen" },
    { "Chinese", "Chinois" },

    // ---- the accent names in theme.cpp ------------------------------------
    { "Purple", "Violet" },
    { "Blue", "Bleu" },
    { "Teal", "Turquoise" },
    { "Green", "Vert" },
    { "Red", "Rouge" },
    { "Pink", "Rose" },
};

std::string g_lang = "en";

// Only built when the UI is not in English: in English every lookup would find
// the string it was handed, so there is nothing to look up.
std::unordered_map<std::string, const char*>* g_table = nullptr;

} // namespace

namespace i18n
{

void load()
{
    g_lang = config::get().language;
    if (g_lang != "fr")
        return;

    g_table = new std::unordered_map<std::string, const char*>();
    g_table->reserve(sizeof(kFr) / sizeof(kFr[0]) * 2);
    for (const auto& e : kFr)
        (*g_table)[e.en] = e.fr;
}

const std::string& lang()
{
    return g_lang;
}

const std::vector<std::string>& langIds()
{
    static const std::vector<std::string> v = { "en", "fr" };
    return v;
}

const std::vector<std::string>& langLabels()
{
    // Endonyms, and never translated: a language picker has to be readable to
    // someone who cannot read the language the UI is currently in.
    static const std::vector<std::string> v = { "English (default)",
                                                "Français" };
    return v;
}

const char* tr(const char* en)
{
    if (!g_table || !en)
        return en;
    auto it = g_table->find(en);
    return it == g_table->end() ? en : it->second;
}

} // namespace i18n
