// Display texts, one line per string: X(id, English, French, Inuktitut). Inuktitut is a DRAFT in syllabics, generated
// by tools/i18n_iu.py from docs/translations/iu.tsv (edit the TSV, not this column; see docs/translations/README.md).
// French is Canadian French (Québec usage: "balayez
// le code QR", "appuyez longuement", IQA "Bonne / Acceptable / Mauvaise", "herbe à poux", "micrologiciel"). Included by i18n.h (ids) and i18n.c (table).
// Adding a language: add one argument to every line (a missing one falls back to English) and a name in i18n.c.
// Format strings must keep the same conversions in the same order in every language.
// (Plain #include on purpose: no include guard.)

// Weather screen, hourly view
X(T_TODAY,          "Today",                       "Aujourd'hui",
                    "ᐅᓪᓗᒥ")
X(T_NOW,            "Now",                         "Actuel",
                    "ᒫᓐᓇ")
X(T_HDR_TEMP,       "Temp",                        "Temp.",
                    "°")
X(T_HDR_RAIN,       "Rain",                        "Pluie",
                    "ᓯᓚᓗᒃ")
X(T_HDR_WIND,       "Wind",                        "Vent",
                    "ᐊᓄᕆ")
X(T_LOADING,        "Loading...",                  "Chargement...",
                    "ᐅᑕᖅᑭᕆᑦᓯ...")
X(T_FEELS,          "Feels %d°",                   "Ressenti %d°",
                    "ᐃᑉᐱᓐᓇᖅᑐᖅ %d°")
X(T_PERCENT,        "%d%%",                        "%d %%",
                    "%d%%")
X(T_RAIN_AROUND,    "Rain around %s",              "Pluie vers %s",
                    "ᓯᓚᓗᓕᖅᑐᖅ %s")
X(T_RAIN_UNTIL,     "Rain until about %s",         "Pluie jusque vers %s",
                    "ᓯᓚᓗᑦᑐᖅ %s ᑎᑭᓪᓗᒍ")
X(T_SNOW_AROUND,    "Snow around %s",              "Neige vers %s",
                    "ᖃᓐᓂᓕᖅᑐᖅ %s")
X(T_SNOW_UNTIL,     "Snow until about %s",         "Neige jusque vers %s",
                    "ᖃᓐᓂᖅᑐᖅ %s ᑎᑭᓪᓗᒍ")
X(T_UNTIL,          "Until %s %s",                 "Jusqu'à %s %s",
                    "%s %s ᑎᑭᓪᓗᒍ")
X(T_ALERT_ALSO,     "\n\nAlso: %s (%s)\n\n%s",     "\n\nAussi : %s (%s)\n\n%s",
                    "\n\nᐊᒻᒪᓗ: %s (%s)\n\n%s")

// Extras page
X(T_DAYLIGHT,       "Daylight\n%d h %02d",         "Durée du jour\n%d h %02d",
                    "ᖃᐅᒪᓂᖅ\n%d h %02d")
X(T_SUNRISE,        "Sunrise\n%s",                 "Lever du soleil\n%s",
                    "ᓯᕿᓂᖅ ᓄᐃᔪᖅ\n%s")
X(T_UV_MAX,         "(max %d)",                    "(max %d)",
                    "(ᐊᖏᓂᕐᐹᖅ %d)")
X(T_UV_INDEX,       "UV index",                    "Indice UV",
                    "UV")
X(T_LOW,            "Low",                         "Faible",
                    "ᒥᑭᔪᖅ")
X(T_MODERATE,       "Moderate",                    "Modéré",
                    "ᐊᑯᙵᓃᑦᑐᖅ")
X(T_HIGH,           "High",                        "Élevé",
                    "ᐊᖏᔪᖅ")
X(T_VERY_HIGH,      "Very high",                   "Très élevé",
                    "ᐊᖏᔪᐊᓗᒃ")
X(T_EXTREME,        "Extreme",                     "Extrême",
                    "ᐅᓗᕆᐊᓇᖅᑐᖅ")
X(T_MOON,           "Moon",                        "Lune",
                    "ᑕᖅᑭᖅ")
X(T_MOON_NEW,       "New moon",                    "Nouvelle lune",
                    "ᓄᑖᖅ")
X(T_MOON_WAX_CR,    "Waxing crescent",             "Premier croissant",
                    "ᐊᖏᓪᓕᖅᑐᖅ")
X(T_MOON_FIRST_Q,   "First quarter",               "Premier quartier",
                    "ᐊᕝᕙᖓ")
X(T_MOON_WAX_GIB,   "Waxing gibbous",              "Gibbeuse croissante",
                    "ᐊᖏᓪᓕᖅᑐᐊᓗᒃ")
X(T_MOON_FULL,      "Full moon",                   "Pleine lune",
                    "ᑕᑕᑦᑐᖅ")
X(T_MOON_WAN_GIB,   "Waning gibbous",              "Gibbeuse décroissante",
                    "ᒥᑭᓪᓕᖅᑐᐊᓗᒃ")
X(T_MOON_LAST_Q,    "Last quarter",                "Dernier quartier",
                    "ᐊᕝᕙᖓ ᑭᖑᓪᓕᖅ")
X(T_MOON_WAN_CR,    "Waning crescent",             "Dernier croissant",
                    "ᒥᑭᓪᓕᖅᑐᖅ")
X(T_AIR_QUALITY,    "Air quality",                 "Qualité de l'air",
                    "ᐊᓂᕐᓴᖅ")
X(T_AQI_GOOD,       "Good",                        "Bonne",
                    "ᐱᐅᔪᖅ")
X(T_AQI_MODERATE,   "Moderate",                    "Acceptable",
                    "ᓈᒻᒪᒃᑐᖅ")
X(T_AQI_SENSITIVE,  "Sensitive groups",            "Groupes sensibles",
                    "ᐋᓐᓂᐊᖅᑐᓄᑦ")
X(T_AQI_UNHEALTHY,  "Unhealthy",                   "Mauvaise",
                    "ᐋᓐᓂᐊᕐᓇᖅᑐᖅ")
X(T_AQI_VERY_UNH,   "Very unhealthy",              "Très mauvaise",
                    "ᐋᓐᓂᐊᕐᓇᖅᑐᐊᓗᒃ")
X(T_AQI_HAZARDOUS,  "Hazardous",                   "Dangereuse",
                    "ᐅᓗᕆᐊᓇᖅᑐᖅ")
X(T_POLLEN,         "Pollen",                      "Pollen",
                    "Pollen")
X(T_POLLEN_ALDER,   "Alder",                       "Aulne",
                    "Alder")
X(T_POLLEN_BIRCH,   "Birch",                       "Bouleau",
                    "Birch")
X(T_POLLEN_GRASS,   "Grass",                       "Graminées",
                    "ᐃᕕᒃ")
X(T_POLLEN_RAGWEED, "Ragweed",                     "Herbe à poux",
                    "Ragweed")

// Weather conditions (WMO codes, weather.c)
X(T_WX_CLEAR,       "Clear sky",                   "Ciel dégagé",
                    "ᓯᓚᖅᑭᖅᑐᖅ")
X(T_WX_MAINLY_CLEAR,"Mainly clear",                "Généralement dégagé",
                    "ᓯᓚᖅᑭᖅᑐᐃᓐᓇᖅ")
X(T_WX_PARTLY,      "Partly cloudy",               "Partiellement nuageux",
                    "ᓄᕗᔭᖃᖅᑐᖅ")
X(T_WX_OVERCAST,    "Overcast",                    "Couvert",
                    "ᓄᕗᔭᔪᖅ")
X(T_WX_FOG,         "Fog",                         "Brouillard",
                    "ᑕᑦᓯᖅᑐᖅ")
X(T_WX_DRIZZLE,     "Drizzle",                     "Bruine",
                    "ᓯᓚᓗᑯᓗᒃ")
X(T_WX_FRZ_DRIZZLE, "Freezing drizzle",            "Bruine verglaçante",
                    "ᓯᓚᓗᑯᓗᒃ ᖁᐊᖅᑐᖅ")
X(T_WX_LIGHT_RAIN,  "Light rain",                  "Pluie faible",
                    "ᓯᓚᓗᑦᑐᑯᓗᒃ")
X(T_WX_RAIN,        "Rain",                        "Pluie",
                    "ᓯᓚᓗᑦᑐᖅ")
X(T_WX_HEAVY_RAIN,  "Heavy rain",                  "Forte pluie",
                    "ᓯᓚᓗᑦᑐᐊᓗᒃ")
X(T_WX_FRZ_RAIN,    "Freezing rain",               "Pluie verglaçante",
                    "ᓯᓚᓗᒃ ᖁᐊᖅᑐᖅ")
X(T_WX_LIGHT_SNOW,  "Light snow",                  "Neige faible",
                    "ᖃᓐᓂᖅᑐᑯᓗᒃ")
X(T_WX_SNOW,        "Snow",                        "Neige",
                    "ᖃᓐᓂᖅᑐᖅ")
X(T_WX_HEAVY_SNOW,  "Heavy snow",                  "Forte neige",
                    "ᖃᓐᓂᖅᑐᐊᓗᒃ")
X(T_WX_SNOW_GRAINS, "Snow grains",                 "Neige en grains",
                    "ᖃᓐᓂᑯᓗᐃᑦ")
X(T_WX_RAIN_SHOWERS,"Rain showers",                "Averses de pluie",
                    "ᓯᓚᓗᑦᑐᖅ")
X(T_WX_SNOW_SHOWERS,"Snow showers",                "Averses de neige",
                    "ᖃᓐᓂᖅᑐᖅ")
X(T_WX_STORM,       "Thunderstorm",                "Orage",
                    "ᑲᓪᓕᖅᑐᖅ")
X(T_WX_STORM_HAIL,  "Thunderstorm, hail",          "Orage, grêle",
                    "ᑲᓪᓕᖅᑐᖅ, ᓇᑕᑦᖁᕐᓇᖅ")

// Status page
X(T_STATUS,         "Status",                      "État",
                    "ᖃᓄᐃᓕᖓᓂᖓ")
X(T_STABLE,         "Stable",                      "Stable",
                    "ᐊᐅᓚᔾᔪᐃᑦᑐᖅ")
X(T_BETA,           "Beta",                        "Bêta",
                    "Beta")
X(T_WIFI_UP,        "Wi-Fi %d dBm  ·  %s  ·  up %s", "Wi-Fi %d dBm  ·  %s  ·  depuis %s",
                    "Wi-Fi %d dBm  ·  %s  ·  %s")
X(T_WIFI_OFFLINE,   "Wi-Fi offline  ·  up %s",     "Wi-Fi hors ligne  ·  depuis %s",
                    "Wi-Fi ᑲᓲᔾᔮᖅᓯᒪᔪᖅ  ·  %s")
X(T_AGE_S,          "%d s",                        "%d s",
                    "%d s")
X(T_AGE_MIN,        "%d min",                      "%d min",
                    "%d min")
X(T_AGE_H,          "%d h %02d",                   "%d h %02d",
                    "%d h %02d")
X(T_AGE_D,          "%d d",                        "%d j",
                    "%d ᐅᓪᓗᐃᑦ")
X(T_SVC_NOT_USED,   "not used yet",                "pas encore utilisé",
                    "ᐊᑐᖅᑕᐅᙱᑦᑐᖅ ᓱᓕ")
X(T_SVC_WAIT_SYNC,  "waiting for sync",            "en attente de synchro",
                    "ᐅᑕᖅᑭᔪᖅ")
X(T_SVC_CHECKING,   "checking...",                 "vérification...",
                    "ᖃᐅᔨᒋᐊᖅᑐᖅ...")
X(T_SVC_IN_A_ROW,   "%d failed tries",             "%d essais ratés",
                    "%d ᑐᒡᓕᕆᔪᑦ")
X(T_SVC_OK_AGO,     "OK %s ago",                   "OK il y a %s",
                    "OK %s ᖄᖏᖅᑐᖅ")
X(T_SVC_NEVER_OK,   "not reached yet",             "pas encore joint",
                    "ᐊᑐᕈᓐᓇᙱᑦᑐᖅ")
X(T_SVC_OFFERS,     "offers %s",                   "offre %s",
                    "%s ᐊᑐᐃᓐᓇᖅ")
X(T_API_FORECAST,   "forecast",                    "prévisions",
                    "ᓯᓚᒧᑦ")
X(T_API_AIR,        "air quality",                 "qualité de l'air",
                    "ᐊᓂᕐᓴᖅ")
X(T_API_TILES,      "tiles",                       "tuiles",
                    "ᓄᓇᙳᐊᑦ")
X(T_API_UPDATES,    "updates",                     "mises à jour",
                    "ᓄᑖᕈᕆᐊᕐᓃᑦ")
X(T_ERR_CONNECT,    "Can't connect",               "Connexion impossible",
                    "ᑲᓱᕈᓐᓇᙱᑦᑐᖅ")
X(T_ERR_TIMEOUT,    "Timed out",                   "Délai dépassé",
                    "ᐱᕕᒃᓴᖅ ᖄᖏᖅᑐᖅ")
X(T_ERR_NO_REPLY,   "No reply",                    "Pas de réponse",
                    "ᑭᐅᔾᔪᐃᑦᑐᖅ")
X(T_ERR_BAD_REPLY,  "Bad response",                "Réponse invalide",
                    "ᑭᐅᔾᔪᑖ ᑕᒻᒪᖅᑐᖅ")
X(T_ERR_EMPTY,      "Empty reply",                 "Réponse vide",
                    "ᑭᐅᔾᔪᑖ ᐃᒪᖃᙱᑦᑐᖅ")

// Updates
X(T_WHATS_NEW,      "What's new",                  "Nouveautés",
                    "ᓄᑖᑦ")
X(T_PILL_UPDATE,    "Update %s",                   "Mise à jour %s",
                    "ᓄᑖᖅ %s")
X(T_PILL_UPDATING,  "Updating %d%%",               "Mise à jour %d %%",
                    "ᓄᑖᕈᕆᐊᖅᑐᖅ %d%%")
X(T_UP_DOWNLOADING, "Downloading... keep it plugged in", "Téléchargement... laissez-le branché",
                    "ᒥᓇᕆᔪᖅ... ᑲᓱᖅᓯᒪᑎᓪᓗᒍ")
X(T_UP_INSTALLED,   "Installed. Restarting...",    "Installée. Redémarrage...",
                    "ᐃᓕᑕᐅᔪᖅ. ᐃᑭᑎᒃᑲᓐᓂᕐᑐᖅ...")
X(T_UP_KEPT,        "Your settings are kept",      "Vos réglages sont conservés",
                    "ᐋᖅᑭᔅᓯᒪᐅᑎᑎᑦ ᐱᒋᔭᐅᔪᑦ")
X(T_UP_DONE,        "Updated",                     "Mise à jour faite",
                    "ᓄᑖᕈᕆᔪᖅ")
X(T_UP_BUSY,        "Updating",                    "Mise à jour",
                    "ᓄᑖᕈᕆᐊᖅᑐᖅ")
X(T_UP_AVAILABLE,   "Update available",            "Nouvelle version",
                    "ᓄᑖᖅ ᐊᑐᐃᓐᓇᖅ")
X(T_UP_YOU_HAVE,    "%s  ·  you have %s",          "%s  ·  vous avez %s",
                    "%s  ·  ᐱᒋᔭᐃᑦ %s")
X(T_INSTALL,        "Install",                     "Installer",
                    "ᐃᓕᓕ")
X(T_OTA_NO_SITE,    "Can't reach the update site", "Site des mises à jour injoignable",
                    "ᓄᑖᕈᕆᕕᒃ ᑲᓱᕈᓐᓇᙱᑦᑐᖅ")
X(T_OTA_NO_IMAGE,   "No app image in the manifest","Aucune image dans le manifeste",
                    "ᐃᓕᑕᒃᓴᖅ ᐱᖃᙱᑦᑐᖅ")
X(T_OTA_NO_START,   "Download failed to start",    "Le téléchargement n'a pas démarré",
                    "ᒥᓇᕐᓂᖅ ᐊᐅᓚᓚᐅᙱᑦᑐᖅ")
X(T_OTA_WRONG,      "Wrong firmware image",        "Mauvaise image de micrologiciel",
                    "ᐃᓕᑕᒃᓴᖅ ᑕᒻᒪᖅᑐᖅ")
X(T_OTA_INTERRUPTED,"Download interrupted",        "Téléchargement interrompu",
                    "ᒥᓇᕐᓂᖅ ᓄᖅᑲᖅᑐᖅ")
X(T_OTA_BAD_SITE,   "Unexpected reply from the update site", "Réponse inattendue du site des mises à jour",
                    "ᓄᑖᕈᕆᕕᒃ ᑭᐅᔾᔪᑎᖓ ᑕᒻᒪᖅᑐᖅ")
X(T_OTA_ROLLED_BACK,"%s was undone: the display restarted before it was confirmed",
                    "%s annulée : l'afficheur a redémarré avant de la confirmer",
                    "%s ᐲᖅᑕᐅᖅᑐᖅ: ᓴᖅᑭᖅᑎᑦᓯᔪᖅ ᐊᐅᓪᓚᖅᓯᒃᑲᓐᓂᓚᐅᖅᑐᖅ")
X(T_OTA_INVALID,    "Downloaded image is invalid", "Image téléchargée invalide",
                    "ᒥᓇᖅ ᐊᑐᔪᓐᓇᙱᑦᑐᖅ")

// Settings screen (display) and the settings QR overlay
X(T_DONE,           "Done",                        "OK",
                    "ᐱᔭᕇᖅᑐᖅ")
X(T_SEC_SCREEN,     "SCREEN",                      "ÉCRAN",
                    "ᐃᒐᓚᐅᔭᖅ")
X(T_SEC_UNITS,      "UNITS",                       "UNITÉS",
                    "ᐆᒃᑑᑎᑦ")
X(T_SEC_MORE,       "MORE",                        "PLUS",
                    "ᓱᓕ")
X(T_DIM_QUIET,      "Dim when quiet",              "Tamiser si calme",
                    "ᑖᓂᑭᓪᓕᖅ")
X(T_WAKE_PICKUP,    "Wake on pick-up",             "Réveil si soulevé",
                    "ᑭᕕᑦᑕᐅᒑᖓᑦ")
X(T_TIMING,         "Timing",                      "Délais",
                    "ᐱᕕᒃᓴᐃᑦ")
X(T_T_SHORT,        "Short",                       "Courts",
                    "ᓇᐃᑦᑐᑦ")
X(T_T_NORMAL,       "Normal",                      "Normaux",
                    "ᓈᒻᒪᒃᑐᑦ")
X(T_T_LONG,         "Long",                        "Longs",
                    "ᑕᑭᔪᑦ")
X(T_T_CUSTOM,       "Custom",                      "Autre",
                    "ᓇᒻᒥᓂᖅ")
X(T_TEMPERATURE,    "Temperature",                 "Température",
                    "ᐆᓇᕐᓂᖅ")
X(T_WIND,           "Wind",                        "Vent",
                    "ᐊᓄᕆ")
X(T_CLOCK,          "Clock",                       "Horloge",
                    "ᐃᑲᕐᕌᑦ")
X(T_LANGUAGE,       "Language",                    "Langue",
                    "ᐅᖃᐅᓯᖅ")
X(T_SEC_SOUND,      "SOUND",                       "SON",
                    "ᓂᐱ")
X(T_CHIME,          "Alert sound",                 "Alerte sonore",
                    "ᐃᓂᕐᑎᕈᑎ ᓂᐱ")
X(T_CHIME_OFF,      "Off",                         "Non",
                    "ᖃᒥᑦᑐᖅ")
X(T_CHIME_RED,      "Red",                         "Rouge",
                    "ᐊᐅᐸᓗᒃᑐᑦ")
X(T_CHIME_ORANGE,   "Orange+",                     "Orange+",
                    "Orange+")
X(T_CHIME_ALL,      "All",                         "Toutes",
                    "ᑕᒪᕐᒥᒃ")
X(T_VOLUME,         "Volume",                      "Volume",
                    "ᓂᐱᖅᑯᖅᑐᓯᒋᐊᕈᑦ")
X(T_TEST_SOUND,     "Test the sound",              "Essayer le son",
                    "ᓈᓚᒍᒃ ᓂᐱ")
X(T_PHONE,          "Location & more (phone)",     "Endroit et plus (tél.)",
                    "ᓱᓕ ᐅᖄᓚᐅᑎᕋᓛᕐᒥ")
X(T_WIFI_NETWORK,   "Wi-Fi network",               "Réseau Wi-Fi",
                    "Wi-Fi ᑲᓱᖃᑎᒌᑦ")
X(T_UPDATES,        "Updates",                     "Mises à jour",
                    "ᓄᑖᑦ")
X(T_RESTART,        "Restart",                     "Redémarrer",
                    "ᐃᑭᑎᒃᑲᓐᓂᕐᓕ")
X(T_CHECK_NOW,      "Check now >",                 "Vérifier >",
                    "ᖃᐅᔨᒋᐊᕐᓕ >")
X(T_CHECKING,       "Checking...",                 "Vérification...",
                    "ᖃᐅᔨᒋᐊᖅᑐᖅ...")
X(T_UP_TO_DATE,     "Up to date",                  "À jour",
                    "ᓄᑖᐅᔪᖅ")
X(T_FAILED,         "Failed",                      "Échec",
                    "ᑕᒻᒪᖅᑐᖅ")
X(T_TAP_AGAIN,      "Tap again",                   "Touchez encore",
                    "ᓇᕿᒃᑲᓐᓂᕐᓕ")
X(T_RESTARTING,     "Restarting...",               "Redémarrage...",
                    "ᐃᑭᑎᒃᑲᓐᓂᕐᑐᖅ...")
X(T_BRIGHTNESS,     "Brightness %d%%",             "Luminosité %d %%",
                    "ᖃᐅᒪᓂᖓ %d%%")
X(T_SETTINGS,       "Settings",                    "Réglages",
                    "ᐋᖅᑭᔅᓯᒪᐅᑎᑦ")
X(T_HINT_TITLE,     "Choose your location",        "Choisissez votre endroit",
                    "ᐃᓂᒋᔭᐃᑦ ᕿᓂᕐᓗᒍ")
X(T_HINT_HELP,      "%s\nScan with your phone to choose\nyour city and other settings.\n\nTap to close",
                    "%s\nBalayez le code avec votre\ntéléphone pour choisir votre\nville et vos réglages.\n\nTouchez pour fermer",
                    "%s\nᐊᔾᔨᓕᐅᕐᓕ ᐅᖄᓚᐅᑎᕋᓛᕐᒧᑦ\nᓄᓇᓖᑦ ᐋᖅᑭᔅᓯᒪᐅᑎᓪᓗ ᕿᓂᕐᓗᒋᑦ.\n\nᓇᕿᓪᓕ: ᒪᑐᓕ")
X(T_GEST_TITLE,     "Getting around",              "Pour naviguer",
                    "ᖃᓄᖅ ᐊᑐᖅᑕᐅᕚ")
X(T_GEST_HELP,      "Swipe sideways: other screens\nDrag up or down: your places\nTap a day: hour by hour\nPress and hold: settings\n\nTap to close",
                    "Glissez de côté : autres écrans\nVers le haut ou le bas : endroits\nTouchez un jour : heure par heure\nAppuyez longuement : réglages\n\nTouchez pour fermer",
                    "ᓂᕈᓗᓪᓕ ᓴᐅᒥᒧᑦ ᑕᓕᕐᐱᒧᓪᓗ: ᐊᓯᖏᑦ\nᖁᓕᒧᑦ ᐊᑖᓄᓪᓗ: ᐃᓂᒋᔭᑎᑦ\nᐅᓪᓗ ᓇᕿᓗᒍ: ᐃᑲᕐᕌᓂ\nᓇᕿᓪᓗᒍ ᓇᕿᒻᒥᓕ: ᐋᖅᑭᔅᓯᒪᐅᑎᑦ\n\nᓇᕿᓪᓕ: ᒪᑐᓕ")
X(T_OV_HELP,        "%s\nScan with your phone and accept\nthe certificate warning.\n\nLong-press for Wi-Fi setup\nTap to close",
                    "%s\nBalayez le code avec votre\ntéléphone et acceptez\nl'avertissement de certificat.\n\nAppuyez longuement : Wi-Fi\nTouchez pour fermer",
                    "%s\nᐊᔾᔨᓕᐅᕐᓕ ᐅᖄᓚᐅᑎᕋᓛᕐᒧᑦ ᐊᒻᒪᓗ\nᐃᓂᕐᑎᕈᑎ ᐊᖏᖅᑕᐅᓕ.\n\nᓇᕿᓪᓗᒍ ᓇᕿᒻᒥᓕ: Wi-Fi\nᓇᕿᓪᓕ: ᒪᑐᓕ")

// Wi-Fi setup screen
X(T_WIFI_RECEIVED,  "Wi-Fi received",              "Wi-Fi reçu",
                    "Wi-Fi ᐱᔭᐅᔪᖅ")
X(T_WIFI_GOT,       "Got \"%s\" from your phone.\nRestarting...", "« %s » reçu du téléphone.\nRedémarrage...",
                    "\"%s\" ᐅᖄᓚᐅᑎᕋᓛᕐᒥᑦ.\nᐃᑭᑎᒃᑲᓐᓂᕐᑐᖅ...")
X(T_WIFI_DPP_FAIL,  "That didn't work. Swipe right\nand join the setup network.",
                    "Échec. Glissez à droite et\njoignez le réseau de configuration.",
                    "ᑕᒻᒪᖅᑐᖅ.\nᓂᕈᓗᓪᓕ ᑕᓕᖅᐱᒧᑦ.")
X(T_WIFI_SETUP,     "Wi-Fi setup",                 "Configuration Wi-Fi",
                    "Wi-Fi ᐋᖅᑭᔅᓯᒪᐅᑎᑦ")
X(T_WIFI_JOIN,      "Scan to join %s\n(password %s).\nAndroid? Swipe left to skip\nthe password",
                    "Balayez pour joindre %s\n(mot de passe %s).\nAndroid? Glissez à gauche\npour éviter le mot de passe",
                    "ᐊᔾᔨᓕᐅᕐᓕ ᑲᓱᕐᓗᒍ %s\n(ᐃᓯᕈᑦ %s).\nAndroid? ᓂᕈᓗᓪᓕ ᓴᐅᒥᒧᑦ\nᐃᓯᕈᑦ ᐲᖅᓗᒍ")
X(T_WIFI_DPP_TITLE, "Android: Easy Connect",       "Android : Easy Connect",
                    "Android: Easy Connect")
X(T_WIFI_DPP_HOW,   "With your phone on your Wi-Fi,\nscan this (camera or any QR app).\nYour phone sends its network.\nSwipe right for other phones",
                    "Téléphone connecté à votre Wi-Fi,\nbalayez ce code (caméra, appli QR).\nVotre téléphone envoie son réseau.\nGlissez à droite : autres téléphones",
                    "ᐊᔾᔨᓕᐅᕐᓕ ᐅᖄᓚᐅᑎᕋᓛᕐᒧᑦ\nWi-Fi-ᒦᑦᑐᒧᑦ.\nᐅᖄᓚᐅᑎᕋᓛᖅ ᑲᓱᖃᑎᒌᓐᓂᒃ ᑐᓃᔪᖅ.\nᓂᕈᓗᓪᓕ ᑕᓕᖅᐱᒧᑦ: ᐊᓯᖏᑦ")
X(T_WIFI_DPP_NONE,  "Easy Connect isn't available.\nSwipe right for other phones.",
                    "Easy Connect n'est pas offert.\nGlissez à droite : autres téléphones.",
                    "Easy Connect ᐊᑐᐃᓐᓇᐅᙱᑦᑐᖅ.\nᓂᕈᓗᓪᓕ ᑕᓕᖅᐱᒧᑦ: ᐊᓯᖏᑦ.")
X(T_TAP_RETRY,      "Tap to try again",            "Touchez pour réessayer",
                    "ᓇᕿᓪᓕ: ᑲᓱᒃᑲᓐᓂᕐᓕ")
X(T_TAP_CANCEL,     "Tap to cancel",               "Touchez pour annuler",
                    "ᓇᕿᓪᓕ: ᖁᔭᓈᕐᓕ")

// Start-up and status messages (main.c)
X(T_WEATHER,        "Weather",                     "Météo",
                    "ᓯᓚ")
X(T_STARTING,       "Starting...",                 "Démarrage...",
                    "ᐱᒋᐊᖅᑐᖅ...")
X(T_CONNECTING,     "Connecting to\n%s\n\nLong-press for Wi-Fi setup", "Connexion à\n%s\n\nAppuyez longuement pour le Wi-Fi",
                    "ᑲᓱᖅᑐᖅ\n%s\n\nᓇᕿᓪᓗᒍ ᓇᕿᒻᒥᓕ: Wi-Fi")
X(T_FETCHING,       "Fetching forecast...\n\nLong-press for Wi-Fi setup", "Prévisions en cours...\n\nAppuyez longuement pour le Wi-Fi",
                    "ᓯᓚᒥᒃ ᖃᐅᔨᓴᖅᑐᖅ...\n\nᓇᕿᓪᓗᒍ ᓇᕿᒻᒥᓕ: Wi-Fi")
X(T_CONNECTED,      "Connected",                   "Connecté",
                    "ᑲᓱᖅᓯᒪᔪᖅ")
X(T_CANT_REACH,     "Can't reach %s\nTap to try again", "%s injoignable\nTouchez pour réessayer",
                    "%s ᑲᓱᕈᓐᓇᙱᑦᑐᖅ\nᓇᕿᓪᓕ: ᑲᓱᒃᑲᓐᓂᕐᓕ")
X(T_FORECAST_RETRY, "Can't reach the forecast service.\nRetrying.",
                    "Service de prévisions injoignable.\nNouvel essai sous peu.",
                    "ᓯᓚᒥᒃ ᖃᐅᔨᓴᕐᕕᒃ ᑲᓱᕈᓐᓇᙱᑦᑐᖅ.\nᑲᓱᒃᑲᓐᓂᖅᑐᖅ.")
X(T_UPDATED_MIN,    "Updated %d min ago",          "Mis à jour il y a %d min",
                    "ᓄᑖᖅᑭᖅᑕᐅᔪᖅ: %d min")
X(T_UPDATED_H,      "Updated %d h ago",            "Mis à jour il y a %d h",
                    "ᓄᑖᖅᑭᖅᑕᐅᔪᖅ: %d h")
X(T_NO_CONNECTION,  "No connection",               "Aucune connexion",
                    "ᑲᓱᕈᓐᓇᙱᑦᑐᖅ")
X(T_STILL_TRYING,   "Can't reach %s\nStill trying\n\nLong-press for Wi-Fi setup",
                    "%s injoignable\nNouvel essai en cours\n\nAppuyez longuement pour le Wi-Fi",
                    "%s ᑲᓱᕈᓐᓇᙱᑦᑐᖅ\nᑲᓱᒃᑲᓐᓂᖅᑐᖅ\n\nᓇᕿᓪᓗᒍ ᓇᕿᒻᒥᓕ: Wi-Fi")
X(T_FIRST_SETUP,    "First-time setup",            "Première configuration",
                    "ᓯᕗᓪᓕᖅ ᐋᖅᑭᒃᓱᐃᓂᖅ")

// Radar
X(T_RADAR,          "Radar",                       "Radar",
                    "Radar")
X(T_RADAR_AT,       "Radar %s  ·  %s",             "Radar %s  ·  %s",
                    "Radar %s  ·  %s")
X(T_RADAR_NOTIME,   "Radar  ·  %s",                "Radar  ·  %s",
                    "Radar  ·  %s")
X(T_PAST_3H,        "Past 3 hours  ·  tap to stop","3 dernières heures  ·  touchez pour arrêter",
                    "3 ᐃᑲᕐᕌᑦ ᖄᖏᖅᑐᑦ  ·  ᓇᕿᓪᓕ ᓄᖅᑲᕐᓗᒍ")
X(T_LOADING_PAST,   "Loading past 3 h... %d/%d",   "Chargement 3 h... %d/%d",
                    "3 ᐃᑲᕐᕌᑦ... %d/%d")
X(T_NO_WIFI,        "No Wi-Fi",                    "Pas de Wi-Fi",
                    "Wi-Fi ᐱᖃᙱᑦᑐᖅ")
X(T_LOADING_RADAR,  "Loading radar...",            "Chargement du radar...",
                    "Radar... ᐅᑕᖅᑭᕆᑦᓯ")
X(T_RADAR_NA,       "Radar unavailable",           "Radar indisponible",
                    "Radar ᐊᑐᐃᓐᓇᐅᙱᑦᑐᖅ")
X(T_PREP_MAPS,      "Preparing maps",              "Préparation des cartes",
                    "ᓄᓇᙳᐊᑦ ᐅᐸᓗᖓᐃᔭᖅᑐᑦ")
X(T_PREP_PROGRESS,  "Downloading radar maps\nzoom level %d of %d\n\n%d / %d tiles",
                    "Téléchargement des cartes\nniveau %d sur %d\n\n%d / %d tuiles",
                    "ᓄᓇᙳᐊᑦ ᒥᓇᕆᔪᑦ\n%d / %d\n\n%d / %d ᓄᓇᙳᐊᑦ")
X(T_LOADING_MAP,    "Loading map %d/%d",           "Chargement de la carte %d/%d",
                    "ᓄᓇᙳᐊᖅ %d/%d")
X(T_MAPS_LOADING,   "Maps still downloading",      "Cartes en téléchargement",
                    "ᓄᓇᙳᐊᑦ ᓱᓕ ᒥᓇᕆᔪᑦ")
X(T_ZOOM_CLOSEST,   "Closest zoom",                "Zoom maximal",
                    "ᐊᖏᓪᓕᒋᐊᕈᓐᓇᙱᑦᑐᖅ")
X(T_ZOOM_WIDEST,    "Widest zoom",                 "Zoom minimal",
                    "ᒥᑭᓪᓕᒋᐊᕈᓐᓇᙱᑦᑐᖅ")
X(T_ZOOM_IN,        "Zoom in",                     "Zoom avant",
                    "ᐊᖏᓪᓕᒋᐊᕐᓕ")
X(T_ZOOM_OUT,       "Zoom out",                    "Zoom arrière",
                    "ᒥᑭᓪᓕᒋᐊᕐᓕ")
