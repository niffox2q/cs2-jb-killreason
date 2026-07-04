#include "jb_killreason.h"
#include <random>
#include <cstdio>
#include <algorithm>

#define MAX_PLAYERS 64

#define CS_TEAM_NONE 0
#define CS_TEAM_SPECTATOR 1
#define CS_TEAM_T 2
#define CS_TEAM_CT 3

jb_killreason g_jb_killreason;
PLUGIN_EXPOSE(jb_killreason, g_jb_killreason);

// SYSTEM API`s
IVEngineServer2* engine = nullptr;
CGlobalVars* gpGlobals = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

// API
IUtilsApi* utils;
IPlayersApi* players_api;
IMenusApi* menus_api;
IJailbreakApi* jailbreak_api;


// VARS
std::map<std::string, std::string> phrases;
int g_iMinimumPrisonersRequired;

struct Reason_t
{
    std::string sReason;
    bool bRespawn;
};
std::unordered_map<std::string,Reason_t> g_mReasons;
std::unordered_map<int,std::vector<int>> g_mGuardKills;


// =========================================
// CONFIG VARS
// =========================================

//==========================================
// HELPERS
//==========================================


// =========================================
// CONFIGS 
// =========================================


void LoadConfig() {
    KeyValues* config = new KeyValues("Config");
    const char* path = "addons/configs/Jailbreak/killreason.ini";
    if (!config->LoadFromFile(g_pFullFileSystem, path)) {
        utils->ErrorLog("%s Failed to load: %s",g_PLAPI->GetLogTag(), path);
        delete config;
        return;
    }

    g_iMinimumPrisonersRequired = config->GetInt("MinPrisoners",4);

    KeyValues* reason = config->FindKey("Reasons");
    if (reason) {
        for (auto kv = reason->GetFirstTrueSubKey(); kv ; kv = kv->GetNextTrueSubKey()){
            std::string keyName = kv->GetName();
            Reason_t data;
            data.sReason = kv->GetString("text","");
            data.bRespawn = kv->GetBool("respawn",false);
            g_mReasons[keyName] = data;
        }
    } else {
        utils->ErrorLog("%s Cant find 'Reason' key in config.",g_PLAPI->GetLogTag());
    }


    delete config;
}

void LoadTranslations() {
    phrases.clear();
    KeyValues* g_kvPhrases = new KeyValues("Phrases");
    const char *pszPath = "addons/translations/jailbreak_killreason.phrases.txt";

    if (!g_kvPhrases->LoadFromFile(g_pFullFileSystem, pszPath))
    {
        utils->ErrorLog("%s Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
        delete g_kvPhrases;
        return;
    }

    const char* language = utils->GetLanguage();

    for (KeyValues *pKey = g_kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey()) {
        phrases[std::string(pKey->GetName())] = std::string(pKey->GetString(language));
    }
    delete g_kvPhrases;
}

const char* GetTranslation(const char* key) {
    auto it = phrases.find(key);
    if (it == phrases.end()) return key;
    else return it->second.c_str();
}

void PrintSlotPrefixed(int iSlot, const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChat(iSlot, buf);
}

void PrintAllPrefixed(const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChatAll(buf);
}

// =========================================
// MAIN
// =========================================
std::vector<int> GetAlivePrisoners(){
    std::vector<int> vResult;
    for (int i = 0 ; i < MAX_PLAYERS; i++) {
        auto pController = CCSPlayerController::FromSlot(i);
        if (!pController || !pController->IsConnected() || pController->GetTeam() != CS_TEAM_T) continue;
        auto pPawn = pController->GetPlayerPawn();
        if (!pPawn || !pPawn->IsAlive()) continue;
        vResult.push_back(i); 
    }
    return vResult;
}

void OpenKRMenu(int iSlot);

void OpenReasonMenu(int iGuard, int iTarget) {
    if (iGuard < 0 || iGuard > MAX_PLAYERS) return;
    auto pController = CCSPlayerController::FromSlot(iGuard);
    if (!pController || pController->GetTeam() != CS_TEAM_CT) {
        PrintSlotPrefixed(iGuard,GetTranslation("OnlyCTCanUse"));
        return;
    }
    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) {
        PrintSlotPrefixed(iGuard,GetTranslation("OnlyAliveCanUse"));
        return;
    }

    Menu hMenu;

    menus_api->SetTitleMenu(hMenu,GetTranslation("ReasonMenuTitle"));

    for (auto &sReason : g_mReasons) {
        menus_api->AddItemMenu(hMenu,sReason.first.c_str(),sReason.second.sReason.c_str(),ITEM_DEFAULT);
    }
    if (g_mReasons.empty()) {
        menus_api->AddItemMenu(hMenu,"",GetTranslation("NoAvailableReasons"),ITEM_DISABLED);
    }

    menus_api->SetBackMenu(hMenu,true);
    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[iTarget](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"back") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            OpenKRMenu(iSlot);
            return;
        }

        if (iItem  < 7) {
            auto gController = CCSPlayerController::FromSlot(iSlot);
            if (!gController || gController->GetTeam() != CS_TEAM_CT) return;

            auto pController = CCSPlayerController::FromSlot(iTarget);
            if (!pController || !pController->IsConnected() || pController->GetTeam() != CS_TEAM_T) {
                auto it = std::find(g_mGuardKills[iSlot].begin(),g_mGuardKills[iSlot].end(),iTarget);
                if (it != g_mGuardKills[iSlot].end()) {
                    g_mGuardKills[iSlot].erase(it);
                    PrintSlotPrefixed(iSlot,GetTranslation("PlayerNotFoundReopening"));
                    OpenKRMenu(iSlot);
                    return;
                }
            }
            auto pPawn = pController->GetPlayerPawn();
            if (!pPawn || pPawn->IsAlive()) {
                auto it = std::find(g_mGuardKills[iSlot].begin(),g_mGuardKills[iSlot].end(),iTarget);
                if (it != g_mGuardKills[iSlot].end()) {
                    g_mGuardKills[iSlot].erase(it);
                    PrintSlotPrefixed(iSlot,GetTranslation("ErrorDetectedPlayerAliveOrUnavailable"));
                    OpenKRMenu(iSlot);
                    return;
                }
            }

            std::string sKeyReason(szBack);

            auto it = g_mReasons.find(sKeyReason);
            if (it == g_mReasons.end()) {
                PrintSlotPrefixed(iSlot,GetTranslation("ErrorWithReason"));
                return;
            } else {
                char msg[512];
                g_SMAPI->Format(msg,sizeof(msg),GetTranslation("KillReasonChat"),gController->GetPlayerName(),pController->GetPlayerName(), it->second.sReason.c_str());
                PrintAllPrefixed(msg);
                if (it->second.bRespawn) {
                    if (GetAlivePrisoners().size() < g_iMinimumPrisonersRequired) {
                        PrintSlotPrefixed(iSlot,GetTranslation("NotRevivedDueNotEnoughtPrisoners"));
                        
                    } else {
                        players_api->Respawn(iTarget);
                        PrintSlotPrefixed(iTarget,GetTranslation("YouRevivedByReason"));
                    }
                }

                for (auto& data : g_mGuardKills){
                    auto& killVector = data.second;
                    killVector.erase(std::remove(killVector.begin(), killVector.end(), iTarget), killVector.end());
                }
            }
        }
        OpenKRMenu(iSlot);
    });
    menus_api->DisplayPlayerMenu(hMenu,iGuard,true,true);
}

void OpenKRMenu(int iSlot) {
    if (iSlot < 0 || iSlot > MAX_PLAYERS) return;
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController || pController->GetTeam() != CS_TEAM_CT) {
        PrintSlotPrefixed(iSlot,GetTranslation("OnlyCTCanUse"));
        return;
    }
    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) {
        PrintSlotPrefixed(iSlot,GetTranslation("OnlyAliveCanUse"));
        return;
    }

    Menu hMenu;

    menus_api->SetTitleMenu(hMenu,GetTranslation("MainMenuTitle"));

    if (!g_mGuardKills[iSlot].empty()) {
        for (auto& victimSlot : g_mGuardKills[iSlot]) {
            auto pController = CCSPlayerController::FromSlot(victimSlot);
            if (!pController || !pController->IsConnected() || pController->GetTeam() != CS_TEAM_T) continue;
            menus_api->AddItemMenu(hMenu,std::to_string(victimSlot).c_str(),pController->GetPlayerName(),ITEM_DEFAULT);
        }
    } else {
        menus_api->AddItemMenu(hMenu,"",GetTranslation("NoAvailablePrisoners"),ITEM_DISABLED);
    }

    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"exit") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }
        if (strcmp(szBack,"") == 0) {
            menus_api->ClosePlayerMenu(iSlot);
            return;
        }

        if (iItem < 7) {
            auto gController = CCSPlayerController::FromSlot(iSlot);
            if (!gController || gController->GetTeam() != CS_TEAM_CT) return;

            int iTarget = atoi(szBack);
            auto pController = CCSPlayerController::FromSlot(iTarget);
            if (!pController || !pController->IsConnected() || pController->GetTeam() != CS_TEAM_T) {
                auto it = std::find(g_mGuardKills[iSlot].begin(),g_mGuardKills[iSlot].end(),iTarget);
                if (it != g_mGuardKills[iSlot].end()) {
                    g_mGuardKills[iSlot].erase(it);
                    PrintSlotPrefixed(iSlot,GetTranslation("PlayerNotFoundReopening"));
                    OpenKRMenu(iSlot);
                    return;
                }
            }
            auto pPawn = pController->GetPlayerPawn();
            if (!pPawn || pPawn->IsAlive()) {
                auto it = std::find(g_mGuardKills[iSlot].begin(),g_mGuardKills[iSlot].end(),iTarget);
                if (it != g_mGuardKills[iSlot].end()) {
                    g_mGuardKills[iSlot].erase(it);
                    PrintSlotPrefixed(iSlot,GetTranslation("ErrorDetectedPlayerAliveOrUnavailable"));
                    OpenKRMenu(iSlot);
                    return;
                }
            }
            OpenReasonMenu(iSlot,iTarget);
        }

        
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
}

bool OnKillReasonCommand(int iSlot, const char* content) {
    OpenKRMenu(iSlot);
    return true;
}

// =========================================
// OTHER
// =========================================


CGameEntitySystem* GameEntitySystem() {
    return utils ? utils->GetCGameEntitySystem() : nullptr;
}



void StartupServer() {
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = utils->GetCEntitySystem();
    gpGlobals = utils->GetCGlobalVars();

    
}

bool jb_killreason::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkSystem, INetworkSystem, NETWORKSYSTEM_INTERFACE_VERSION);

    ConVar_Register(FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);
    g_SMAPI->AddListener(this, this);

    return true;
}



void jb_killreason::AllPluginsLoaded() {
    int ret;
    utils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }


    menus_api = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    players_api = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }


    jailbreak_api =(IJailbreakApi*)g_SMAPI->MetaFactory(JAILBREAK_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Jailbreak Core plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    LoadConfig();
    LoadTranslations();


    utils->StartupServer(g_PLID, StartupServer);

    utils->HookEvent(g_PLID,"player_death",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iAttacker = pEvent->GetInt("attacker");
        auto cAttacker = CCSPlayerController::FromSlot(iAttacker);
        if (!cAttacker || cAttacker->GetTeam() != CS_TEAM_CT) return;
        int iVictim = pEvent->GetInt("userid");
        auto cVictim = CCSPlayerController::FromSlot(iVictim);
        if (!cVictim || cVictim->GetTeam() != CS_TEAM_T) return;
        g_mGuardKills[iAttacker].push_back(iVictim);
        PrintSlotPrefixed(iAttacker,GetTranslation("YouCanEnterKillReason"));
    });

    utils->HookEvent(g_PLID,"round_prestart",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        g_mGuardKills.clear();
    });

    utils->RegCommand(g_PLID,{"mm_killreason","mm_kr"},{"!killreason","!kr"},OnKillReasonCommand);

    utils->HookEvent(g_PLID,"player_disconnect",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iSlot = pEvent->GetInt("userid");
        for (auto& data : g_mGuardKills){
            auto& killVector = data.second;
            killVector.erase(std::remove(killVector.begin(), killVector.end(), iSlot), killVector.end());
        }
    });

    utils->HookEvent(g_PLID,"player_team",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iSlot = pEvent->GetInt("userid");
        for (auto& data : g_mGuardKills){
            auto& killVector = data.second;
            killVector.erase(std::remove(killVector.begin(), killVector.end(), iSlot), killVector.end());
        }
    });

    utils->HookEvent(g_PLID,"player_spawn",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iSlot = pEvent->GetInt("userid");
        for (auto& data : g_mGuardKills){
            auto& killVector = data.second;
            killVector.erase(std::remove(killVector.begin(), killVector.end(), iSlot), killVector.end());
        }
    });

    
}

bool jb_killreason::Unload(char* error, size_t maxlen) {
    jailbreak_api->ClearAllPluginHooks(g_PLID);
    utils->ClearAllHooks(g_PLID);
    ConVar_Unregister();

   
    return true;
}

const char* jb_killreason::GetAuthor() { return "niffox"; }
const char* jb_killreason::GetDate() { return __DATE__; }
const char* jb_killreason::GetDescription() { return "[JB] Kill Reason"; }
const char* jb_killreason::GetLicense() { return "GPL"; }
const char* jb_killreason::GetLogTag() { return "[JB] Kill Reason"; }
const char* jb_killreason::GetName() { return "[JB] Kill Reason"; }
const char* jb_killreason::GetURL() { return "https://t.me/niffox_2q"; }
const char* jb_killreason::GetVersion() { return "1.0.1"; }