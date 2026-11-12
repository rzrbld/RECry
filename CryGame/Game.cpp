#if defined(CRY_PORT_UNIX)
#include "../port/platform/PortTiming.h"
#include <chrono>
#include <thread>
#endif
//////////////////////////////////////////////////////////////////////
//
//	Game source code
//	
//	File: Game.cpp
// 
//	History:
//	-August 02,2001: Create by Alberto Demichelis
//	-Sep 24,2001 : Modified by Petar Kotevski
//
//////////////////////////////////////////////////////////////////////
 
#include "StdAfx.h"

#include <IStreamEngine.h>
#include <ICryPak.h>
#include "Game.h"
#include "XNetwork.h"
#include "XServer.h"
#include "XClient.h"

#include "UIHud.h"

#include "WeaponSystemEx.h"
#include "WeaponClass.h"

#include "XVehicleSystem.h"
#include "PlayerSystem.h"
#include "XServer.h"

#include "XSystemBase.h"

#include "UISystem.h"
#include "UIScreen.h"
#include "Flock.h" 

#include "EntityClassRegistry.h"
#include "ScriptObjectGame.h"
#include "ScriptObjectInput.h"

#include "ScriptObjectLanguage.h"
#include "ScriptObjectPlayer.h"
#include "ScriptObjectSpectator.h"					// CScriptObjectSpectator
#include "ScriptObjectAdvCamSystem.h"				// CScriptObjectAdvCamSystem
#include "ScriptObjectSynched2DTable.h"			// CScriptObjectSynched2DTable
#include "ScriptObjectVehicle.h"
#include "ScriptObjectRenderer.h"
#include "ScriptObjectStream.h"
#include "ScriptObjectWeaponClass.h"
#include "ScriptObjectAI.h"

#include "ScriptObjectUI.h"

#include "ScriptObjectBoids.h"
#include "ScriptTimerMgr.h"

#include "IngameDialog.h"
#include <IEntitySystem.h>
#include <ISound.h>

#include "TagPoint.h"
//#include "XPath.h"

#include "XPlayer.h"
#include "XVehicle.h"

#include <IMovieSystem.h>
#include "CMovieUser.h"

#include "TimeDemoRecorder.h"
#include "GameMods.h"

#ifdef WIN32
#include <winioctl.h>
#include <tchar.h>

typedef std::basic_string< TCHAR > tstring;
typedef std::vector< TCHAR > tvector;
#endif
 
//////////////////////////////////////////////////////////////////////////
// Pointer to Global ISystem.
static ISystem* gISystem = 0;
__attribute__((weak)) ISystem* GetISystem()
{
	return gISystem;
}

//////////////////////////////////////////////////////////////////////////////
// Linux port: deterministic gameplay start + state tracing for automated
// XTest/headless verification. Enabled only through environment variables so
// normal gameplay is unaffected.
//////////////////////////////////////////////////////////////////////////////
static bool PortEnvFlag(const char *szName)
{
	const char *szValue = getenv(szName);
	return szValue && *szValue && strcmp(szValue, "0") != 0;
}

void CXGame::PortDebugTraceGameplay()
{
	if (!PortEnvFlag("FARCRY_TRACE_GAMEPLAY"))
		return;

	ITimer *pTimer = m_pSystem ? m_pSystem->GetITimer() : NULL;
	float fNow = pTimer ? pTimer->GetAsyncCurTime() : 0.0f;
	static float fLastTrace = -1000.0f;
	if (fNow - fLastTrace < 1.0f)
		return;
	fLastTrace = fNow;

	const char *szFocus = "-";
	if (m_pUISystem)
	{
		CUIScreen *pFocus = m_pUISystem->GetFocusScreen();
		if (pFocus)
			szFocus = (const char *)pFocus->GetName().c_str();
	}

	IEntity *pMyPlayer = GetMyPlayer();
	Vec3 vPlayerPos(0, 0, 0);
	if (pMyPlayer)
		vPlayerPos = pMyPlayer->GetPos();
	IProcess *pProc = m_pSystem ? m_pSystem->GetIProcess() : NULL;
	int nProcFlags = pProc ? pProc->GetFlags() : 0;
	IConsole *pConsole = m_pSystem ? m_pSystem->GetIConsole() : NULL;
	Vec3 vCam = m_pSystem ? m_pSystem->GetViewCamera().GetPos() : Vec3(0, 0, 0);
	EntityId nPlayerId = m_pClient ? m_pClient->GetPlayerId() : (EntityId)0;

	CryLogAlways("PORTGAME state t=%.2f level='%s' mission='%s' menu=%d uioverlay=%d proc=%p flags=%x console=%d loadingFromFile=%d client=%p connected=%d playerId=%u myPlayer=%p playerPos=(%.1f %.1f %.1f) cam=(%.1f %.1f %.1f) camAngles=(%.1f %.1f %.1f) curUI=%p hud=%p focus='%s'",
		fNow, m_currentLevel.c_str(), m_currentMission.c_str(),
		m_bMenuOverlay ? 1 : 0, m_bUIOverlay ? 1 : 0,
		(void *)pProc, nProcFlags,
		(pConsole && pConsole->IsOpened()) ? 1 : 0,
		m_bIsLoadingLevelFromFile ? 1 : 0,
		(void *)m_pClient,
		(m_pClient && m_pClient->IsConnected()) ? 1 : 0,
		(unsigned)nPlayerId,
		(void *)pMyPlayer, vPlayerPos.x, vPlayerPos.y, vPlayerPos.z,
		vCam.x, vCam.y, vCam.z,
        m_pSystem->GetViewCamera().GetAngles().x, m_pSystem->GetViewCamera().GetAngles().y, m_pSystem->GetViewCamera().GetAngles().z,
		(void *)m_pCurrentUI, (void *)m_pUIHud, szFocus);

    CPlayer* tracedPlayer = NULL;
    if (pMyPlayer && pMyPlayer->GetContainer() &&
        pMyPlayer->GetContainer()->QueryContainerInterface(CIT_IPLAYER,(void**)&tracedPlayer) && tracedPlayer)
        CryLogAlways("PORTGAME weapon clip=%d reserve=%d firing=%d reloading=%d", tracedPlayer->m_stats.ammo_in_clip, tracedPlayer->m_stats.ammo, (int)tracedPlayer->m_stats.firing, (int)tracedPlayer->m_stats.reloading);

	if (m_pScriptSystem)
	{
		_SmartScriptObject pLocalPlayer(m_pScriptSystem, true);
		bool bLocalPlayer = m_pScriptSystem->GetGlobalValue("_localplayer", pLocalPlayer) != 0;
		CryLogAlways("PORTGAME localplayer_script=%d hudDisplay=%d", bLocalPlayer ? 1 : 0,
			(m_pClient && m_pClient->m_bDisplayHud) ? 1 : 0);
        const char* className = "<missing>";
        if (bLocalPlayer) pLocalPlayer->GetValue("classname",className);
        _SmartScriptObject hud(m_pScriptSystem,true);
        bool hide = false;
        if (m_pScriptSystem->GetGlobalValue("Hud",hud)) hud->GetValue("bHide",hide);
        ICVar* hudVar=m_pSystem->GetIConsole()->GetCVar("cl_display_hud");
        CryLogAlways("PORTGAME identity name='%s' nativeClass='%s' luaClass='%s' hide=%d hudCvar=%d",pMyPlayer?pMyPlayer->GetName():"?",pMyPlayer?pMyPlayer->GetEntityClassName():"?",className?className:"?",hide,hudVar?hudVar->GetIVal():-1);

	}

	// Input pipeline state: keyboard key state, action map enable/activation.
	IInput *pInput = m_pSystem ? m_pSystem->GetIInput() : NULL;
	int nKeyW = pInput ? (int)pInput->GetKeyState(XKEY_W) : 0;
	int nKeyEsc = pInput ? (int)pInput->GetKeyState(XKEY_ESCAPE) : 0;
	bool bMapEnabled = m_pIActionMapManager && m_pIActionMapManager->IsEnabled();
	bool bForward = m_pIActionMapManager && m_pIActionMapManager->CheckActionMap("MOVE_FORWARD");
	bool bBack = m_pIActionMapManager && m_pIActionMapManager->CheckActionMap("MOVE_BACKWARD");
	bool bJump = m_pIActionMapManager && m_pIActionMapManager->CheckActionMap("JUMP");
	CryLogAlways("PORTGAME input keyW=%d keyEsc=%d actionMap=%p enabled=%d forward=%d back=%d jump=%d",
		nKeyW, nKeyEsc, (void *)m_pIActionMapManager, bMapEnabled ? 1 : 0, bForward ? 1 : 0, bBack ? 1 : 0, bJump ? 1 : 0);
}

void CXGame::PortDebugStartGameplay()
{
	if (!PortEnvFlag("FARCRY_START_GAMEPLAY"))
		return;

	static bool bFinished = false;
	static float fFirstTick = -1.0f;
	static float fLastTry = -1000.0f;
	if (bFinished || !m_pClient || !m_pServer || !m_p3DEngine || m_currentLevel.empty() || !m_pSystem)
		return;

	ITimer *pTimer = m_pSystem->GetITimer();
	float fNow = pTimer ? pTimer->GetAsyncCurTime() : 0.0f;
	if (fFirstTick < 0.0f)
		fFirstTick = fNow;
	if (fNow - fLastTry < 1.0f)
		return;
	fLastTry = fNow;

	// Bind the server-assigned local player to the client if the network
	// handshake has not already done it (CXClient::SetPlayerID).
	EntityId nClientPlayer = m_pClient->GetPlayerId();
	if (nClientPlayer == 0 || nClientPlayer == INVALID_WID)
	{
		CXServer::XSlotMap &mapSlots = m_pServer->GetSlotsMap();
		for (CXServer::XSlotMap::iterator it = mapSlots.begin(); it != mapSlots.end(); ++it)
		{
			CXServerSlot *pSlot = it->second;
			if (pSlot && pSlot->GetPlayerId() != 0 && pSlot->GetPlayerId() != INVALID_WID)
			{
				CryLogAlways("PORTGAME START_GAMEPLAY: binding client player %u from server slot %u",
					(unsigned)pSlot->GetPlayerId(), (unsigned)it->first);
				m_pClient->ForceConnected();
				m_pClient->SetPlayerID(pSlot->GetPlayerId());
				break;
			}
		}
	}
	if (!m_pClient->IsConnected())
		m_pClient->ForceConnected();

	// Close the loading screen and any cutscene/menu overlay, then hand input
	// back to the 3d engine process.
	m_bMenuOverlay = false;
	m_bUIOverlay = false;
	m_p3DEngine->Enable(1);
	if (m_pUISystem)
	{
		m_pUISystem->StopAllVideo();
		if (m_pUISystem->GetScriptObjectUI())
			m_pUISystem->GetScriptObjectUI()->OnSwitch(false);
	}
	if (m_pUIHud)
	{
		m_pUIHud->Reset();
		SetCurrentUI(m_pUIHud);
	}
	IConsole *pConsole = m_pSystem->GetIConsole();
	if (pConsole)
	{
		pConsole->ResetProgressBar(0);
		pConsole->ShowConsole(false);
		if (!getenv("FARCRY_KEEP_CUTSCENES")) // test: let level-start cutscenes play
			pConsole->ExecuteString("SkipCutScene");
	}
	if (m_pSystem->GetIMovieSystem() && !getenv("FARCRY_KEEP_CUTSCENES"))
		m_pSystem->GetIMovieSystem()->Reset(true);
	m_pSystem->SetIProcess(m_p3DEngine);
	if (m_pSystem->GetIProcess())
		m_pSystem->GetIProcess()->SetFlags(PROC_3DENGINE);
	if (m_pSystem->GetIInput())
		m_pSystem->GetIInput()->EnableEventPosting(1);

	IEntity *pMyPlayer = GetMyPlayer();
	Vec3 vPos(0, 0, 0);
	if (pMyPlayer)
		vPos = pMyPlayer->GetPos();
	bool bPlayerBound = m_pClient->GetPlayerId() != 0 && m_pClient->GetPlayerId() != INVALID_WID;
	CryLogAlways("PORTGAME START_GAMEPLAY t=%.2f playerId=%u playerBound=%d myPlayer=%p pos=(%.1f %.1f %.1f) attempts=%.1f",
		fNow, (unsigned)m_pClient->GetPlayerId(), bPlayerBound ? 1 : 0,
		(void *)pMyPlayer, vPos.x, vPos.y, vPos.z, fNow - fFirstTick);

	if (bPlayerBound || (fNow - fFirstTick) > 10.0f)
	{
		bFinished = true;
		CryLogAlways("PORTGAME START_GAMEPLAY finished playerBound=%d after %.1fs", bPlayerBound ? 1 : 0, fNow - fFirstTick);
	}
}


//////////////////////////////////////////////////////////////////////////////////////////////
// DLL Interface

//////////////////////////////////////////////////////////////////////
// interface of the DLL
IGame* CreateGameInstance()
{
	CXGame *pGame = new CXGame();
	return pGame;
}

//////////////////////////////////////////////////////////////////////////////////////////////
// CTagPointManager

class CTagPointManager : public ITagPointManager
{
public:
	CTagPointManager( IGame *pGame )  { m_pGame = (CXGame*) pGame; };
	virtual ~CTagPointManager() {};

	// This function creates a tag point in the game world
	virtual ITagPoint *CreateTagPoint(const string &name, const Vec3 &pos, const Vec3 &angles) 
	{
		return m_pGame->CreateTagPoint( name, pos, angles );
	};

	// Retrieves a tag point by name
	virtual ITagPoint *GetTagPoint(const string &name)
	{
		return m_pGame->GetTagPoint( name );
	}

	// Deletes a tag point from the game
	virtual void RemoveTagPoint(ITagPoint *pPoint)
	{
		m_pGame->RemoveTagPoint( pPoint );
	}

	virtual void AddRespawnPoint(ITagPoint *pPoint)
	{
		m_pGame->AddRespawnPoint( pPoint );
	}

	virtual void RemoveRespawnPoint(ITagPoint *pPoint)
	{
		m_pGame->RemoveRespawnPoint( pPoint );
	}

private:
	CXGame *m_pGame;
};

//////////////////////////////////////////////////////////////////////////////////////////////
// CXGame
//!constructor
CXGame::CXGame()
{
	m_pTimeDemoRecorder = NULL;
	m_pScriptObjectGame = NULL;
	m_pScriptTimerMgr = NULL;
	m_pScriptSystem = NULL;
	m_pServer = NULL;
	m_pClient = NULL;
	m_pLog = NULL;
	m_pServerSnooper = NULL;
	m_pNETServerSnooper = 0;
	m_pRConSystem = 0;
	m_pWeaponSystemEx = NULL;
	m_mapTagPoints.clear();
	m_bMenuInitialized = false;
	m_pCurrentUI = 0;	
	m_pIActionMapManager=NULL;
	m_pIngameDialogMgr = new CIngameDialogMgr();
	m_pUISystem = 0;
	mp_model = 0;
	// to avoid all references to movie user in this file
	m_pMovieUser = new CMovieUser(this); // port: created on Linux too (in-engine cutscenes)
	m_nPlayerIconTexId = -1;
	m_nVehicleIconTexId = -1;
	m_nBuildingIconTexId = -1;
	m_nUnknownIconTexId = -1;
	m_bMenuOverlay = false;
	m_bUIOverlay = false;
	m_bUIExclusiveInput = false;
	m_bHideLocalPlayer = false;
	m_pCVarCheatMode=NULL;

	m_fTimeGran=m_fFixedStep=m_fTimeGran2FixedStep=m_frFixedStep = 0;
	m_iFixedStep2TimeGran = 0;
	g_language=0;
	g_playerprofile=0;
	g_serverprofile=0;

	m_tPlayerPersistentData.m_bDataSaved=false;
	m_fFadingStartTime=-1.0f;
	cv_game_physics_quality=NULL;
	m_bMapLoadedFromCheckpoint=false;
	m_bSynchronizing = false;

	//m_fTimeToSaveThumbnail = 0;
	m_pGameMods = NULL;
	m_bLastDoLateSwitch = 0;
	m_bLastCDAuthentication = 0;
	m_bAllowQuicksave = true;

	m_sGameName = "FarCry";
	m_pTagPointManager = new CTagPointManager( this );
	m_nDEBUG_TIMING = 0;
	m_fDEBUG_STARTTIMER = 0;
}

//////////////////////////////////////////////////////////////////////
//!destructor
CXGame::~CXGame()
{
#if defined(CRY_PORT_UNIX)
    if (m_p3DEngine) m_p3DEngine->SetRenderCallback(NULL, NULL);
#endif
	m_pScriptSystem->BeginCall("Shutdown");
	m_pScriptSystem->PushFuncParam(0);
	m_pScriptSystem->EndCall();

	if (m_pIngameDialogMgr)
		delete m_pIngameDialogMgr;
	m_pIngameDialogMgr=NULL;

	if (m_pMovieUser)
	{
		if (m_pSystem)
		{
			IMovieSystem *pMovieSystem=m_pSystem->GetIMovieSystem();
			if (pMovieSystem)
			{
				if (pMovieSystem->GetUser()==m_pMovieUser)
					pMovieSystem->SetUser(NULL);
			}
		}
		delete m_pMovieUser;
	}
	m_pMovieUser=NULL;

	// shutdown the client if there is one
	ShutdownClient();

	// shutdown the server if there is one
	ShutdownServer();

	if (m_pUISystem)
	{
		m_pUISystem->Release();
	}
	SAFE_DELETE(m_pUISystem);


	CScriptObjectUI::ReleaseTemplate();
	CScriptObjectPlayer::ReleaseTemplate();
	CScriptObjectFireParam::ReleaseTemplate();
	CScriptObjectWeaponClass::ReleaseTemplate();
	CScriptObjectVehicle::ReleaseTemplate();
	CScriptObjectSpectator::ReleaseTemplate();
	CScriptObjectAdvCamSystem::ReleaseTemplate();
	CScriptObjectSynched2DTable::ReleaseTemplate();
	CScriptObjectBoids::ReleaseTemplate();
	CScriptObjectRenderer::ReleaseTemplate();
	CScriptObjectGame::ReleaseTemplate();
	CScriptObjectInput::ReleaseTemplate();
	CScriptObjectLanguage::ReleaseTemplate();

	CScriptObjectAI::ReleaseTemplate();
	CScriptObjectServer::ReleaseTemplate();
	CScriptObjectServerSlot::ReleaseTemplate();
	CScriptObjectClient::ReleaseTemplate();
	CScriptObjectStream::ReleaseTemplate();

	cl_scope_flare->Release();
	cl_ThirdPersonRange->Release();

	cl_ThirdPersonOffs->Release();
	cl_ThirdPersonOffsAngHor->Release();
	cl_ThirdPersonOffsAngVert->Release();

	cl_display_hud->Release();
  cl_motiontracker->Release();
  cl_hud_pickup_icons->Release();
  cl_msg_notification->Release();
  cl_hud_name->Release();
	ai_num_of_bots->Release();
	p_name->Release();
	p_model->Release();
	mp_model->Release();
	p_color->Release();
	p_always_run->Release();
	g_language->Release();
	g_playerprofile->Release();
	g_serverprofile->Release();
	g_GC_Frequence->Release();
	p_speed_run->Release();
	p_sprint_scale->Release();

	p_sprint_decoy->Release();
	p_sprint_restore_run->Release();
	p_sprint_restore_idle->Release();

	p_speed_walk->Release();
	p_speed_crouch->Release();
	p_speed_prone->Release();
	p_jump_force->Release();
	p_jump_run_time->Release();
	p_jump_walk_time->Release();
//	p_lean->Release();
	p_lean_offset->Release();
	p_bob_pitch->Release();
	p_bob_roll->Release();
	p_bob_length->Release();
	p_bob_weapon->Release();
	p_bob_fcoeff->Release();
	p_weapon_switch->Release();

	cv_game_physics_quality->Release();

	m_jump_vel->Release();
	m_jump_arc->Release();

	b_camera->Release();

//	f_update->Release();
//	f_draw->Release();
//	f_drawDbg->Release();

	sv_timeout->Release();
	cl_timeout->Release();
	cl_loadtimeout->Release();

	g_LevelName->Release();
	g_GameType->Release();
	g_LeftHanded->Release();

	p_DeadBody->Release();
	p_HitImpulse->Release();
	p_RotateHead->Release();
	a_DrawArea->Release();
	a_LogArea->Release();

	m_pCVarCheatMode->Release();

	pl_JumpNegativeImpulse->Release();

	if (m_pRenderer && (m_nPlayerIconTexId>=0))
		m_pRenderer->RemoveTexture(m_nPlayerIconTexId);
	if (m_pRenderer && (m_nVehicleIconTexId>=0))
		m_pRenderer->RemoveTexture(m_nVehicleIconTexId);
	if (m_pRenderer && (m_nBuildingIconTexId>=0))
		m_pRenderer->RemoveTexture(m_nBuildingIconTexId);
	if (m_pRenderer && (m_nUnknownIconTexId>=0))
		m_pRenderer->RemoveTexture(m_nUnknownIconTexId);
	
	SAFE_DELETE(m_pUIHud);
	SAFE_DELETE(m_pWeaponSystemEx);
	SAFE_DELETE(m_pVehicleSystem);
	SAFE_DELETE(m_pPlayerSystem);

	SAFE_DELETE(m_pFlockManager);

	CScriptObjectPlayer::ReleaseTemplate();
	
	//shutdown script stuff
	SAFE_DELETE(m_pScriptObjectGame);	
	SAFE_DELETE(m_pScriptObjectInput);

	SAFE_DELETE(m_pScriptObjectBoids);
	SAFE_DELETE(m_pScriptObjectLanguage);
	SAFE_DELETE(m_pScriptObjectAI);


	// Release the action map
	SAFE_RELEASE(m_pIActionMapManager);
	SAFE_DELETE(m_pScriptTimerMgr);
	// release the tags
	if (!m_mapTagPoints.empty())
	{
		TagPointMap::iterator ti;
		for (ti=m_mapTagPoints.begin();ti!=m_mapTagPoints.end();ti++)
			delete ti->second;
	}

	SAFE_RELEASE(m_pServerSnooper);
	SAFE_RELEASE(m_pNETServerSnooper);
	SAFE_RELEASE(m_pRConSystem);
	SAFE_DELETE(m_pTimeDemoRecorder);
	SAFE_DELETE(m_pGameMods);

	delete m_pTagPointManager;
}

//////////////////////////////////////////////////////////////////////////
bool CXGame::InitClassRegistry()
{
	m_EntityClassRegistry.Init( m_pSystem );
	CPlayerSystem *pPlayerSystem = GetPlayerSystem();
	CVehicleSystem *pVehicleSystem = GetVehicleSystem();
	CWeaponSystemEx *pWeaponSystemEx = GetWeaponSystemEx();	// m10

	assert( pPlayerSystem );
	assert( pVehicleSystem );
	assert( pWeaponSystemEx );

	// Enumerate entity classes.
	EntityClass *entCls = NULL;
	m_EntityClassRegistry.MoveFirst();
	do {
		entCls = m_EntityClassRegistry.Next();
		if (entCls)
		{
			const char* entity_type = entCls->strGameType.c_str();
			EntityClassId ClassId = entCls->ClassId;
			if(strcmp("Player",entity_type)==0)
				pPlayerSystem->AddPlayerClass(ClassId);

			if(strcmp("Vehicle",entity_type)==0)
				pVehicleSystem->AddVehicleClass(ClassId);

			if(strcmp("Projectile",entity_type)==0)
			{
				// cannot be loaded at that point - other scripts must be loaded before
				pWeaponSystemEx->AddProjectileClass(ClassId);
			}
		}
	} while (entCls);
	return true;
}

//////////////////////////////////////////////////////////////////////////
void CXGame::SoftReset()
{
	m_pLog->Log("Soft Reset Begin");
	//allow to reload scripts with(LoadScript)
	m_pScriptSystem->SetGlobalToNull("_localplayer");
	if(m_pScriptSystem)
		m_pScriptSystem->UnloadScripts();

	std::vector<string> vLoadedWeapons;

	for (int i = 0; i != m_pWeaponSystemEx->GetNumWeaponClasses(); ++i)
		vLoadedWeapons.push_back(m_pWeaponSystemEx->GetWeaponClass(i)->GetName());

	m_pWeaponSystemEx->Reset();
	if (m_pSystem->GetIMovieSystem())
		m_pSystem->GetIMovieSystem()->StopAllSequences();
	m_pScriptObjectGame->Reset();

	m_pScriptSystem->ForceGarbageCollection();
	//m_p3DEngine->ClearRenderResources();

	m_iLastCmdIdx = 0;

	m_pWeaponSystemEx->Init(this);

	for (std::vector<string>::iterator i = vLoadedWeapons.begin(); i != vLoadedWeapons.end(); ++i)
		AddWeapon((*i).c_str());

	//if (GetMyPlayer())
	//	GetMyPlayer()->SetNeedUpdate(true);

	if (m_pCurrentUI) 
	{
		m_pCurrentUI->Reset();
		m_pCurrentUI->Init(m_pScriptSystem);
	}

	m_pLog->Log("Soft Reset End");
}

//////////////////////////////////////////////////////////////////////
//!reset the game before a level reloading
//!this function allow the reloading of all scripts
//!and wipe out all textures from the 3dengine
void CXGame::Reset()
{
	m_pEntitySystem->Reset();

	// Unload all music.
	if (m_pSystem->GetIMusicSystem())
		m_pSystem->GetIMusicSystem()->Unload();

	//allow to reload scripts with(LoadScript)
	m_pScriptSystem->SetGlobalToNull("_localplayer");
	if(m_pScriptSystem)
		m_pScriptSystem->UnloadScripts();

	m_pWeaponSystemEx->Reset();
	m_XSurfaceMgr.Reset();
	m_XAreaMgr.Clear();
	ClearTagPoints();
	if (m_pSystem->GetIMovieSystem())
		m_pSystem->GetIMovieSystem()->Reset(false);
	m_pScriptObjectGame->Reset();

	m_pScriptSystem->ForceGarbageCollection();
	m_p3DEngine->ClearRenderResources();

	// Must reset all timers.
	m_pScriptTimerMgr->Reset();

	//clen up the input buffer
	if (m_pSystem->GetIInput())
	{
		m_pSystem->GetIInput()->Update(true);
		m_pSystem->GetIInput()->Update(true);
	}
	if (m_pIActionMapManager)
		m_pIActionMapManager->Reset();

	m_iLastCmdIdx = 0;

	//////////////////////////////////////////////////////////////////////////
	// Reset UI.
	//////////////////////////////////////////////////////////////////////////
	if (m_pUISystem)
	{
		m_pUISystem->UnloadAllModels();
		m_pUISystem->StopAllVideo();
		m_p3DEngine->Enable(1);
		//m_pSystem->GetILog()->Log("UISystem: Enabled 3D Engine!");
	}
	if (m_pCurrentUI) 
		m_pCurrentUI->Reset();
	if (m_pUIHud)
		m_pUIHud->Reset();

	if (GetMyPlayer())
		GetMyPlayer()->SetNeedUpdate(true);
}

IXSystem *CXGame::GetXSystem(){return m_pServer?m_pServer->m_pISystem:m_pClient?m_pClient->m_pISystem:NULL;}
//////////////////////////////////////////////////////////////////////
//! Initialize the game. This must be called before calling other functions of this class.
bool CXGame::Init(struct ISystem *pSystem,bool bDedicatedSrv,bool bInEditor,const char *szGameMod)
{	
	// Setup the system and 3D Engine pointers
	m_pSystem	= pSystem;

	m_pGameMods = new CGameMods(this);

	gISystem = pSystem;
	m_bDedicatedServer=bDedicatedSrv;
	m_XAreaMgr.Init( pSystem );
	m_bEditor=bInEditor;


	m_bRelaunch=false;
	m_bMovieSystemPaused = false;
	m_bIsLoadingLevelFromFile=false;
	
	m_bOK = false;
	m_bUpdateRet = true;
	m_pClient	= NULL;
	m_pServer	= NULL;

  m_pSystem->GetILog()->Log("Game Initialization");
	// Port: the movie system exists on Linux now (in-engine cutscenes).
	IMovieSystem *pMovieSystem=m_pSystem->GetIMovieSystem();
	if (pMovieSystem)
		pMovieSystem->SetUser(m_pMovieUser);
	if (!m_pTimeDemoRecorder)
		m_pTimeDemoRecorder = new CTimeDemoRecorder(pSystem);
  
	m_pUIHud = NULL;
	m_pNetwork= m_pSystem->GetINetwork();
	m_pLog= m_pSystem->GetILog();
	m_p3DEngine	= m_pSystem->GetI3DEngine();
	m_pRenderer = m_pSystem->GetIRenderer();
	m_pScriptSystem=pSystem->GetIScriptSystem();
	m_pEntitySystem=m_pSystem->GetIEntitySystem();

	// Register game rendering callback.
	// The local first-person entity is intentionally removed from spatial
    // sectors by CPlayer::Update. Submit it at the scene callback instead.
#if defined(CRY_PORT_UNIX)
    m_p3DEngine->SetRenderCallback([](void* context) {
        if (([]{ static const bool b = getenv("FARCRY_DIAGNOSTIC_NO_PLAYER_DRAW") != NULL; return b; }())) return;
        CXGame* game = static_cast<CXGame*>(context);
        IEntity* entity = game->GetMyPlayer();
        if (!entity || !entity->GetContainer() || entity->IsHidden()) return;
        CPlayer* player = NULL;
        if (!entity->GetContainer()->QueryContainerInterface(CIT_IPLAYER,(void**)&player) || !player || !player->m_bFirstPerson) return;
        // CPlayer::OnDraw draws the first-person hands and weapon from here only
        // indoors (lit by the ambient at the player's position); outdoors the 3D
        // engine's pass draws them. Two copies flickered while walking.
        extern bool g_PortFPWeaponFromCallback;
        g_PortFPWeaponFromCallback = true;
        SRendParams params;
        params.vPos = entity->GetPos();
        params.vAngles = entity->GetAngles();
        params.vAmbientColor = game->GetSystem()->GetI3DEngine()->GetAmbientColorFromPosition(entity->GetPos());
        player->OnDraw(params);
        g_PortFPWeaponFromCallback = false;
    }, this);
#endif
	
	// init subsystems
#ifndef _XBOX
	m_pServerSnooper=m_pNetwork->CreateServerSnooper(this);
	m_pNETServerSnooper=m_pNetwork->CreateNETServerSnooper(this);
	m_pRConSystem=m_pNetwork->CreateRConSystem();
#endif
	m_pWeaponSystemEx = new CWeaponSystemEx();
	m_pVehicleSystem = new CVehicleSystem();
	m_pPlayerSystem = new CPlayerSystem();
	m_pFlockManager = new CFlockManager(m_pSystem);

	CScriptObjectUI::InitializeTemplate(m_pScriptSystem);

	// init is not necessary for now, but add here if it later is
	m_pScriptObjectGame=new CScriptObjectGame;
	CScriptObjectGame::InitializeTemplate(m_pScriptSystem);

	m_pScriptObjectInput=new CScriptObjectInput;
	CScriptObjectInput::InitializeTemplate(m_pScriptSystem);
	m_pScriptObjectLanguage=new CScriptObjectLanguage;
	CScriptObjectLanguage::InitializeTemplate(m_pScriptSystem);
	m_pScriptObjectBoids = new CScriptObjectBoids;
	CScriptObjectBoids::InitializeTemplate(m_pScriptSystem);
	m_pScriptObjectAI = new CScriptObjectAI;
	CScriptObjectAI::InitializeTemplate(m_pScriptSystem);
	CScriptObjectServer::InitializeTemplate(m_pScriptSystem);

	CScriptObjectPlayer::InitializeTemplate(m_pScriptSystem);
	CScriptObjectFireParam::InitializeTemplate(m_pScriptSystem);
	CScriptObjectWeaponClass::InitializeTemplate(m_pScriptSystem);
	CScriptObjectVehicle::InitializeTemplate(m_pScriptSystem);
	CScriptObjectSpectator::InitializeTemplate(m_pScriptSystem);
	CScriptObjectAdvCamSystem::InitializeTemplate(m_pScriptSystem);
	CScriptObjectSynched2DTable::InitializeTemplate(m_pScriptSystem);
	CScriptObjectRenderer::InitializeTemplate(m_pScriptSystem);


	m_pScriptObjectGame->Init(m_pScriptSystem, this);
	m_pScriptObjectInput->Init(m_pScriptSystem,this,m_pSystem);


	m_pScriptObjectBoids->Init(m_pScriptSystem,m_pSystem,m_pFlockManager);
	m_pScriptObjectLanguage->Init(m_pScriptSystem,&m_StringTableMgr);
	m_pScriptObjectAI->Init(m_pScriptSystem,m_pSystem,this);

	CScriptObjectServerSlot::InitializeTemplate(m_pScriptSystem);
	CScriptObjectClient::InitializeTemplate(m_pScriptSystem);
	CScriptObjectStream::InitializeTemplate(m_pScriptSystem);
	
	m_pScriptTimerMgr=new CScriptTimerMgr(m_pScriptSystem,m_pSystem->GetIEntitySystem(),this);

// making some constants accessable to the script
	m_pScriptSystem->SetGlobalValue("FireActivation_OnPress",ePressing);
	m_pScriptSystem->SetGlobalValue("FireActivation_OnRelease",eReleasing);
	m_pScriptSystem->SetGlobalValue("FireActivation_OnHold",eHolding);
	
	m_pScriptSystem->SetGlobalValue("ENTITYTYPE_PLAYER", ENTITYTYPE_PLAYER);
	m_pScriptSystem->SetGlobalValue("ENTITYTYPE_WAYPOINT", ENTITYTYPE_WAYPOINT);
	m_pScriptSystem->SetGlobalValue("ENTITYTYPE_OWNTEAM", ENTITYTYPE_OWNTEAM);

/*	m_pScriptSystem->SetGlobalValue("CMD_GO", CMD_GO);
	m_pScriptSystem->SetGlobalValue("CMD_ATTACK", CMD_ATTACK);
	m_pScriptSystem->SetGlobalValue("CMD_DEFEND", CMD_DEFEND);
	m_pScriptSystem->SetGlobalValue("CMD_COVER", CMD_COVER);
	m_pScriptSystem->SetGlobalValue("CMD_BARRAGEFIRE", CMD_BARRAGEFIRE);*/

	InitConsoleVars();

	if (szGameMod && szGameMod[0])
	{
		// apply the mod without restarting as the game just started!
		GetModsInterface()->SetCurrentMod(szGameMod,false);
	}
	
	InitClassRegistry();
		
	// execute the "main"-script (to pre-load other scripts, etc.)
	m_pScriptSystem->ExecuteFile("scripts/main.lua");
	// The retail main.lua reload uses a lowercase path that can miss on a
	// case-sensitive pak; load the sound preset DB explicitly so sound-preset
	// entities have their table.
	m_pScriptSystem->ExecuteFile("Scripts/Sounds/PresetDB.lua");
	m_pScriptSystem->ExecuteFile("Scripts/Sounds/EAXPresetDB.lua");
	// UI:PlayMusic() checks these Lua globals; the NULL audio backends never
	// registered them, so menu music silently never started.
	m_pScriptSystem->SetGlobalValue("s_MusicEnable", 1);
	m_pScriptSystem->SetGlobalValue("s_MusicVolume", 1.0f);
	m_pScriptSystem->SetGlobalValue("s_SFXVolume", 1.0f);
	m_pScriptSystem->BeginCall("Init");
	m_pScriptSystem->PushFuncParam(0);
	m_pScriptSystem->EndCall();

	// initialize the surface-manager
	m_XSurfaceMgr.Init(m_pScriptSystem,m_p3DEngine,GetSystem()->GetIPhysicalWorld());
	
	// init key-bindings
	if(!m_bDedicatedServer)
		InitInputMap();

	// create various console-commands/variables
	InitConsoleCommands();
	
	// loading the main language-string-table
	if (!m_StringTableMgr.Load(GetSystem(),*m_pScriptObjectLanguage,g_language->GetString()))
		m_pLog->Log("cannot load language file [%s]",g_language->GetString());
		
	// creating HUD interface
	m_pLog->Log("Initializing UI");
	m_pUIHud = new CUIHud(this,m_pSystem);

	LoadConfiguration("","game.cfg");

	//////////////////////////////////////////////////////////////////////////
	// Materials
	// load materials (once before all, this info stays till we quit the game - no need to load material later)
	// first load normal materials
	m_XSurfaceMgr.LoadMaterials("scripts/materials");
 
	if(!m_bDedicatedServer)
	{
		m_pSystem->GetIConsole()->ShowConsole(0);
		if (!bInEditor)
		{
			//------------------------------------------------------------------------------------------------- 
			m_pUISystem = new CUISystem;

			if (m_pUISystem)
			{
				m_pUISystem->Create(this, m_pSystem, m_pScriptSystem, "Scripts/MenuScreens/UISystem.lua", 1);
			}
			else
			{
				m_pLog->Log("Failed to create UI System!");
			}
			//------------------------------------------------------------------------------------------------- 
		}

		//------------------------------------------------------------------------------------------------- 
		if (m_pUISystem)
		{
			m_bMenuOverlay = 1;
		}
		//------------------------------------------------------------------------------------------------- 
	}
	else
		m_pSystem->GetIConsole()->ShowConsole(true);

	// load textures used as icons by the mini-map
	if (m_pRenderer)
	{
		ITexPic *pPic;
		pPic=m_pRenderer->EF_LoadTexture("gui/map_player", FT_NOREMOVE, 0, eTT_Base);
		if (pPic && pPic->IsTextureLoaded())
			m_nPlayerIconTexId=pPic->GetTextureID();
		pPic=m_pRenderer->EF_LoadTexture("gui/map_vehicle", FT_NOREMOVE, 0, eTT_Base);
		if (pPic && pPic->IsTextureLoaded())
			m_nVehicleIconTexId=pPic->GetTextureID();
		pPic=m_pRenderer->EF_LoadTexture("gui/map_building", FT_NOREMOVE, 0, eTT_Base);
		if (pPic && pPic->IsTextureLoaded())
			m_nBuildingIconTexId=pPic->GetTextureID();
		pPic=m_pRenderer->EF_LoadTexture("gui/map_unknown", FT_NOREMOVE, 0, eTT_Base);
		if (pPic && pPic->IsTextureLoaded())
			m_nUnknownIconTexId=pPic->GetTextureID();
	}

	if(!bInEditor)
		m_pEntitySystem->SetDynamicEntityIdMode(true);	

#if !defined(_XBOX) && !defined(PS2) && !defined(LINUX)
	SetCursor(NULL);
#endif

	//////////////////////////////////////////////////////////////////////////
	
	DevModeInit();
	m_bOK = true;	
	e_deformable_terrain = NULL;

	return (true);
}

//////////////////////////////////////////////////////////////////////
//! game-mainloop
bool CXGame::Run(bool &bRelaunch)
{
    //_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_LEAK_CHECK_DF);
	
	if(m_bDedicatedServer)
	{
		return Update();
	}
	else 
	{
		//static float fLastFrame=0;
		m_bRelaunch=false;
		while(1) 
		{		
			if (!Update()) 
				break;
		}

		bRelaunch=m_bRelaunch;
	}
	return true;
}

#if !defined(_XBOX) && !defined(PS2) && !defined(LINUX)
#include <Mmsystem.h>
#include "Game.h"
#pragma comment (lib , "Winmm.lib")
#else
#define GetCurrentTime() ((unsigned int)(GetSystem()->GetITimer()->GetCurrTime() * 1000.f))
#endif

//////////////////////////////////////////////////////////////////////////
bool CXGame::IsInPause(IProcess *pProcess)
{
	bool bPause = (m_bMenuOverlay && (!IsMultiplayer()));
	//check if the game is in pause or in menu mode
	if ((pProcess->GetFlags() & PROC_MENU) && !IsMultiplayer())
		bPause=true;

	return (bPause);
}

//////////////////////////////////////////////////////////////////////
//! update all game and children
bool g_PortMenuActive = false;
double g_PortPhaseMs[PORT_PHASE_COUNT];
double g_PortGameplayStartMs = 0;
void PortSamplerPoll();
int PortSleepFarRigidBodies(IPhysicalWorld *pWorld, const vectorf &center, float dist, float vmax);

int g_PortMuzzleFrames = 0;
Vec3 g_PortImpactPos(0,0,0);
int g_PortImpactFrames = 0;

bool CXGame::Update()
{
	// The engine shows menus through m_bMenuOverlay (GotoMenu/GotoGame) and the
	// UI overlay flag; menu screens stay instantiated during play, so an active
	// screen count does not mean a menu is visible (it blocked stick look/strafe).
	g_PortMenuActive = (m_bMenuOverlay || m_bUIOverlay);
	// Benchmark placement: FARCRY_BENCH_POS="x y z [yaw]" moves the local player
	// once, 2 s after it exists outside menus (repeatable outdoor/combat views).
	PortSamplerPoll();
	// Handheld physics budget: rigid bodies far from the player that are only
	// creeping (parked vehicles still settling, debris) are put to sleep. At
	// low frame rates large steps kept them jittering awake for good, and
	// vehicle contact checks then cost more than the whole renderer (Rebellion
	// on the H700: 16 FPS, physics 44% of the main thread). Anything moving
	// fast or near the player is untouched; driving or a hit wakes it again.
	// Part of HANDHELD_PHYSICS (default on); FARCRY_FAR_PHYSICS_SLEEP overrides,
	// FARCRY_FAR_PHYSICS_DIST sets the distance in metres.
	if (g_PortGameplayStartMs != 0 && GetMyPlayer())
	{
		static const bool bFarSleep = getenv("FARCRY_FAR_PHYSICS_SLEEP") ? atoi(getenv("FARCRY_FAR_PHYSICS_SLEEP")) != 0 : !(getenv("HANDHELD_PHYSICS") && atoi(getenv("HANDHELD_PHYSICS")) == 0);
		static const float fFarDist = getenv("FARCRY_FAR_PHYSICS_DIST") ? (float)atof(getenv("FARCRY_FAR_PHYSICS_DIST")) : 60.0f;
		static double s_fNextFarSleep = 0;
		IPhysicalWorld* pWorld = m_pSystem->GetIPhysicalWorld();
		if (bFarSleep && pWorld && PortNowMs() >= s_fNextFarSleep)
		{
			s_fNextFarSleep = PortNowMs() + 500.0;
			const Vec3 vPlayer = GetMyPlayer()->GetPos();
			static int s_nSlept = 0;
			const int n = PortSleepFarRigidBodies(pWorld, vPlayer, fFarDist, 2.0f);
			if (n && s_nSlept < 20) { s_nSlept += n; CryLogAlways("PORT far physics: %d body(ies) put to sleep", n); }
		}
	}
	if (g_PortGameplayStartMs == 0 && !m_bMenuOverlay && !m_bIsLoadingLevelFromFile && GetMyPlayer())
	{
		g_PortGameplayStartMs = PortNowMs();
		CryLogAlways("PORT gameplay start (bench time base)");
		if (const char* szCv = getenv("FARCRY_PRINT_CVARS")) // diagnostic: "name1,name2"
		{
			std::string list(szCv); size_t p = 0;
			while (p <= list.size())
			{
				size_t c = list.find(',', p); std::string n = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
				if (ICVar* v = m_pSystem->GetIConsole()->GetCVar(n.c_str())) fprintf(stderr, "PORT cvar %s = %s\n", n.c_str(), v->GetString());
				if (c == std::string::npos) break; p = c + 1;
			}
		}
	}
	// Port: skip movies and cutscenes with the pad (A, B or Start), as Esc,
	// Space and F7 do on a keyboard. In-engine cutscenes run SkipCutScene;
	// the Bink movie player (CutScenePlayer.lua, patched in memory by the
	// script system) reads g_PortPadSkip and keeps its own "can skip" rules.
	{
		extern bool g_PortPadSkip; // CryInput: pad A/B/Start pressed this frame
		const bool bSkip = g_PortPadSkip;
		if (IScriptSystem* pSS = m_pSystem->GetIScriptSystem())
			pSS->SetGlobalValue("g_PortPadSkip", bSkip ? 1 : 0);
		if (bSkip && m_pMovieUser && m_pMovieUser->IsInCutScene())
			m_pSystem->GetIConsole()->ExecuteString("SkipCutScene");
	}
	if (const char* szBench = getenv("FARCRY_BENCH_POS"))
	{
		static int s_nBench = 0; static double s_fBenchT = 0;
		IEntity* pBenchPlayer = (s_nBench < 2 && !m_bMenuOverlay) ? GetMyPlayer() : NULL;
		if (pBenchPlayer)
		{
			if (s_nBench == 0) { s_nBench = 1; s_fBenchT = PortNowMs(); }
			else if (PortNowMs() - s_fBenchT > 2000.0)
			{
				float x, y, z, yaw = 0;
				if (sscanf(szBench, "%f %f %f %f", &x, &y, &z, &yaw) >= 3)
				{
					// Never start below the terrain: the player would fall through.
					if (m_p3DEngine)
					{
						const float fGround = m_p3DEngine->GetTerrainElevation(x, y);
						if (z < fGround + 1.0f && !([]{ static const bool b = getenv("FARCRY_BENCH_NOCLAMP") != NULL; return b; }())) z = fGround + 1.0f; // NOCLAMP: caves, under water
						const float fWater = m_p3DEngine->GetWaterLevel();
						if (fGround < fWater)
							CryLogAlways("PORT bench: WARNING ground %.1f is below water level %.1f at (%.1f %.1f)", fGround, fWater, x, y);
					}
					pBenchPlayer->SetPos(Vec3(x, y, z));
					pBenchPlayer->SetAngles(Vec3(0, 0, yaw));
					CryLogAlways("PORT bench: player placed at (%.1f %.1f %.1f) yaw %.1f", x, y, z, yaw);
				}
				s_nBench = 2;
			}
		}
		// Bench motion (testing): FARCRY_BENCH_SPIN=<deg/s> turns, FARCRY_BENCH_WALK=<m/s> moves
		// along the initial yaw, both steadily from the placement on.
		if (s_nBench == 2 && GetMyPlayer())
		{
			static const float fSpin = getenv("FARCRY_BENCH_SPIN") ? (float)atof(getenv("FARCRY_BENCH_SPIN")) : 0.f;
			static const float fWalk = getenv("FARCRY_BENCH_WALK") ? (float)atof(getenv("FARCRY_BENCH_WALK")) : 0.f;
			if (fSpin != 0.f || fWalk != 0.f)
			{
				// Engine frame time, so "fixed_time_step" gives the same motion per frame
				// however slow a capture makes the frame.
				const float dt = m_pSystem->GetITimer()->GetFrameTime();
				float x = 0, y = 0, z = 0, yaw0 = 0; sscanf(szBench, "%f %f %f %f", &x, &y, &z, &yaw0);
				IEntity* pP = GetMyPlayer();
				// Absolute heading (placement yaw + spin so far): scripts cannot turn the bench camera.
				static float s_fYaw = 1e9f; if (s_fYaw > 1e8f) s_fYaw = yaw0;
				s_fYaw += fSpin * dt;
				Vec3 a = pP->GetAngles(); a.x = 0; a.z = s_fYaw; pP->SetAngles(a);
				if (fWalk != 0.f)
				{
					const float r = yaw0 * 3.14159265f / 180.f; Vec3 pos = pP->GetPos();
					pos.x += -sinf(r) * fWalk * dt; pos.y += cosf(r) * fWalk * dt; pP->SetPos(pos);
				}
			}
		}
	}
	// Test-only: spawn a named particle effect near the player.
	if (const char* fx = getenv("FARCRY_TEST_EFFECT"))
	{
		static float lastFx = -1000.0f;
		ITimer* pT = m_pSystem ? m_pSystem->GetITimer() : NULL;
		float now = pT ? pT->GetAsyncCurTime() : 0.0f;
		if (now - lastFx > 1.0f)
		{
			lastFx = now;
			if (m_pSystem && m_p3DEngine)
			{
				CCamera& cam = m_pSystem->GetViewCamera();
				Vec3 fwd = -cam.GetVCMatrixD3D9().GetOrtZ();
				Vec3 pos = cam.GetPos() + fwd*3.0f;
				IParticleEffect* pEff = m_p3DEngine->FindParticleEffect(fx);
				if (pEff) { pEff->Spawn(pos, fwd, 1.0f); CryLogAlways("PORT effect spawned: %s fwd=(%.2f %.2f %.2f)", fx, fwd.x, fwd.y, fwd.z); }
				else CryLogAlways("PORT effect not found: %s", fx);
			}
		}
	}
	// Linux port: optional deterministic gameplay start / state trace.
	PortDebugStartGameplay();
	PortDebugTraceGameplay();

	if (!m_nDEBUG_TIMING)
	{
		m_fDEBUG_STARTTIMER = m_pSystem->GetITimer()->GetAsyncCurTime();
		m_nDEBUG_TIMING = 1;
	}

	if (!m_bEditor)
	{
		if (!m_bMenuOverlay || !m_pUISystem || m_pUISystem->GetScriptObjectUI()->CanRenderGame())
		{
			m_p3DEngine->Enable(1);
		}
		else
		{
			m_p3DEngine->Enable(0);
		}
	}

	bool bRenderFrame = (!m_pSystem->GetViewCamera().GetPos().IsZero() || m_bMenuOverlay || m_bUIOverlay) 
											&& g_Render->GetIVal() != 0;

	//////////////////////////////////////////////////////////////////////////
	// Start Profiling frame
	m_pSystem->GetIProfileSystem()->StartFrame();
	//////////////////////////////////////////////////////////////////////////
	FUNCTION_PROFILER( m_pSystem,PROFILE_GAME );

	ITimer *pTimer=m_pSystem->GetITimer();
	pTimer->MeasureTime("EnterGameUp");
	//Timur[10/2/2002]
	// Cannot Run Without System.
	assert( m_pSystem );

#if defined(CRY_PORT_UNIX)
    // Pace against a monotonic deadline, not last frame's delta (which makes
    // sleep alternate between long and short frames). Avoid queuing unbounded
    // render work on the driver when the handheld target is 30 FPS.
    typedef std::chrono::steady_clock FrameClock;
    static FrameClock::time_point nextFrame;
    static float previousLimit = 0;
    float maxFPS = g_maxfps->GetFVal();
    if (maxFPS > 0 && maxFPS <= 1000) {
        FrameClock::time_point now = FrameClock::now();
        // Keep the schedule through small overruns (catch up on the next
        // frames) so the average reaches the target; resync only after a
        // real stall of more than two frames.
        const FrameClock::duration period = std::chrono::duration_cast<FrameClock::duration>(std::chrono::duration<double>(1.0 / maxFPS));
        if (previousLimit != maxFPS || nextFrame + 2 * period < now) nextFrame = now;
        if (now < nextFrame) std::this_thread::sleep_until(nextFrame);
        nextFrame += std::chrono::duration_cast<FrameClock::duration>(std::chrono::duration<double>(1.0 / maxFPS));
        previousLimit = maxFPS;
    } else previousLimit = 0;
#else
	float	maxFPS=g_maxfps->GetFVal();
	if(maxFPS>0)
	{
//		float	extraTime = pTimer->GetAsyncCurTime() + 1.0f/maxFPS - pTimer->GetFrameTime();
//		while(extraTime - pTimer->GetAsyncCurTime() > 0)
//			;
		DWORD	extraTime = (DWORD)((1.0f/maxFPS - pTimer->GetFrameTime())*1000.0f);
#if !defined(LINUX)
		if(extraTime>0&&extraTime<300)//thread sleep not process sleep
			Sleep(extraTime);
#endif
	}

#endif

	PhysicsVars *pVars = m_pSystem->GetIPhysicalWorld()->GetPhysVars();
	float fTimeGran=pVars->timeGranularity,	fFixedStep=g_MP_fixed_timestep->GetFVal();
	if (fTimeGran!=m_fTimeGran || fFixedStep!=m_fFixedStep)
	{
		m_fTimeGran = fTimeGran; m_fFixedStep = fFixedStep;
		if (fFixedStep>0)
		{
			m_fTimeGran2FixedStep = fTimeGran/fFixedStep;
			m_iFixedStep2TimeGran = (int)(fFixedStep/fTimeGran+0.5f);
			m_frFixedStep = 1.0f/fFixedStep;
		}
		else
			m_iFixedStep2TimeGran = 0;
		m_frTimeGran = 1.0f/fTimeGran;
	}
	pVars->bMultiplayer = IsMultiplayer() ? 1:0;

	//check what is the current process 
	//bool bPause=false;
	IProcess *pProcess=m_pSystem->GetIProcess();
	if (!pProcess)
		return false;

	bool bPause=IsInPause(pProcess);
	if (m_bIsLoadingLevelFromFile)
		bPause=false;
	// Pauses or unpauses movie system.
	if (bPause != m_bMovieSystemPaused)
	{
		m_bMovieSystemPaused = bPause;
		if (bPause)
			m_pSystem->GetIMovieSystem()->Pause();
		else
			m_pSystem->GetIMovieSystem()->Resume();
	}
	// [marco] check current sound and vis areas
	// for music etc.	
	CheckSoundVisAreas();

	//int nStartGC=m_pScriptSystem->GetCGCount();
	pTimer->MeasureTime("SomeStuff");
	// update system
	int nPauseMode=0;
	if (bPause)
		nPauseMode=1;
	// TODO: Add pause mode for a cutscene currently playing (pausemode 2)

	if (IsMultiplayer()) {
		pe_params_flags pf; pf.flagsOR = pef_update;
		for(ListOfPlayers::iterator	pl=m_DeadPlayers.begin(); pl!=m_DeadPlayers.end(); pl++)
			if ((*pl)->GetEntity() && (*pl)->GetEntity()->GetPhysics())
				(*pl)->GetEntity()->GetPhysics()->SetParams(&pf);
	}
	
	bool bPortSysOk;
	{
		SPortPhaseScope portScope(PORT_PHASE_SYS); // physics, AI, entities, sound
		bPortSysOk = m_pSystem->Update(IsMultiplayer() ? ESYSUPDATE_MULTIPLAYER:0, nPauseMode);
	}
	if (!bPortSysOk) //Update returns false when quitting
		return (false);
	double fPortGameStart = PortNowMs(), fPortRenderStart = 0;

	if (IsMultiplayer()) {
		pe_params_flags pf; pf.flagsAND = ~pef_update;
		for(ListOfPlayers::iterator	pl=m_DeadPlayers.begin(); pl!=m_DeadPlayers.end(); pl++)
			if ((*pl)->GetEntity() && (*pl)->GetEntity()->GetPhysics())
				(*pl)->GetEntity()->GetPhysics()->SetParams(&pf);
	}

	pTimer->MeasureTime("SysUpdate");
	
	// [marco] after system update, retrigger areas if necessary
	if (!bPause)
		RetriggerAreas();

	if (!bPause || (m_pClient && !m_pClient->IsConnected()))
	{	// network start
		FRAME_PROFILER( "GameUpdate:Client",m_pSystem,PROFILE_GAME );

		// update client
		if (m_pClient)
		{
			m_pClient->UpdateClientNetwork();
			pTimer->MeasureTime("Net");

			assert(m_pClient);
			m_pClient->Update();
			
			if(m_pClient->DestructIfMarked())			//  to make sure the client is only released in one place - here
				m_pClient=0;
		}

		pTimer->MeasureTime("ClServ Up");

		////////UPDATE THE NETWORK 
		// [Anton] moved from after the rendering, so that the server has access to the most recent physics data
		// update server
		if (m_pServer)
		{
			FRAME_PROFILER( "GameUpdate:Server",m_pSystem,PROFILE_GAME );

			m_pServer->Update();
		}

		pTimer->MeasureTime("EndServUp");
	}

	m_pNetwork->UpdateNetwork();	// used to update things like the UBI.com services

	DWORD dwCurrentTimeInMS=GetCurrentTime();

	if (!m_pSystem->IsDedicated())
	{
		if(m_pServerSnooper)
			m_pServerSnooper->Update(dwCurrentTimeInMS);

		if(m_pNETServerSnooper)
			m_pNETServerSnooper->Update(dwCurrentTimeInMS);
	}

	if(m_pRConSystem)
	{
		if (m_pClient)
			m_pRConSystem->Update( dwCurrentTimeInMS,m_pClient->GetNetworkClient() );
		else
			m_pRConSystem->Update(dwCurrentTimeInMS);
	}

	// network end

	// system rendering
	if (bRenderFrame)
	{	
		// render begin must be always called anyway to clear buffer, draw buttons etc.
		// even in menu mode
		g_PortPhaseMs[PORT_PHASE_GAME] += PortNowMs() - fPortGameStart; // client/server/game logic
		fPortRenderStart = PortNowMs();
		m_pSystem->RenderBegin();
		
		m_pSystem->Render();
		pTimer->MeasureTime("3SysRend");
	}
		
	// update the HUD	
	if (m_pCurrentUI && !bPause && m_pClient && m_pClient->m_bDisplayHud)		
	{
		FRAME_PROFILER( "GameUpdate:HUD",m_pSystem,PROFILE_GAME );

		// update hud itself
		if(!m_pCurrentUI->Update())
			m_bUpdateRet = false;

    // update ingame-dialog-manager
		if (m_pIngameDialogMgr)
			m_pIngameDialogMgr->Update();
	
    pTimer->MeasureTime("HUD Up");
	}

	if (m_pUISystem && m_pUISystem->IsEnabled())
	{
		FRAME_PROFILER("GameUpdate:UI", m_pSystem, PROFILE_GAME);

		if (m_bMenuOverlay || m_bUIOverlay)
		{
			m_pUISystem->Update();			
			m_pUISystem->Draw();
		}
	}

	if(a_DrawArea->GetIVal())
  {
		m_XAreaMgr.DrawAreas( m_pSystem );
    pTimer->MeasureTime("XAreaDraw");
  }

	 // print time profiling results
#ifndef PS2
	pTimer->MeasureTime((const char*)-1);
#else
	pTimer->MeasureTime("Time");
#endif

	pTimer->MeasureTime("3TimeProf");

	//NETWORK DEBUGGING
	if(m_pClient && m_pClient->cl_netstats->GetIVal()!=0)
	{
		m_pClient->DrawNetStats();
	}
	if(m_pServer && m_pServer->sv_netstats->GetIVal()!=0)
	{
		m_pServer->DrawNetStats(m_pRenderer);
	}

	pTimer->MeasureTime("NetStats");

	// end of frame
	if (bRenderFrame)
  {	
		// same thing as for render begin		
		if (m_pTimeDemoRecorder)
			m_pTimeDemoRecorder->RenderInfo();

		if (fPortRenderStart > 0) g_PortPhaseMs[PORT_PHASE_RENDER] += PortNowMs() - fPortRenderStart; // scene + UI submission
		{
			SPortPhaseScope portScope(PORT_PHASE_END); // renderer end of frame incl. swap
			m_pSystem->RenderEnd();
		}
	}
  pTimer->MeasureTime("3Rend Up");
	
	// get messages from process
	//char *szProcessMessage = m_pSystem->GetIProcess()->GetPMessage();
	// process messages from process
	{
		if (m_fFadingStartTime>0)
		{
			if (((m_pSystem->GetITimer()->GetCurrTime())-m_fFadingStartTime)>1.5f)
			{
				m_fFadingStartTime=-1;
				SendMessage(m_szLoadMsg);
			}
		}

		while(!m_qMessages.empty())
		{
			string smsg=m_qMessages.front();
			m_qMessages.pop();
			ProcessPMessages(smsg.c_str());		
		}

		// the messages can switch the game to menu or viceversa
		bPause=IsInPause(pProcess);
	}
	
	//update script timers
	if(m_pScriptTimerMgr)
		m_pScriptTimerMgr->Update( (unsigned long)(pTimer->GetCurrTime()*1000) );
	
	pTimer->MeasureTime("ScrTimerUp");

	if(g_GC_Frequence->GetFVal()>0)
	{
		// Change Script Garbage collection frequency.
		m_pSystem->SetGCFrequency(g_GC_Frequence->GetFVal());
	}

	//////////////////////////////////////////////////////////////////////////
	// Special update function for developers mode.
	//////////////////////////////////////////////////////////////////////////
	if (IsDevModeEnable())		
		DevModeUpdate();
	//////////////////////////////////////////////////////////////////////////

	//////////////////////////////////////////////////////////////////////////

	pTimer->MeasureTime("EndGameUp");

	//////////////////////////////////////////////////////////////////////////
	// End Profiling Frame
	m_pSystem->GetIProfileSystem()->EndFrame();
	//////////////////////////////////////////////////////////////////////////

	return (m_bUpdateRet);
}

void CXGame::UpdateDuringLoading()
{
/*	// to find areas where the function should be called more often
	{
		static DWORD dwTime=GetTickCount();

		DWORD dwNewTime=GetTickCount();

		DWORD dwRelative = dwNewTime-dwTime;

		if(dwRelative>1000)
			m_pSystem->GetILog()->Log(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> UpdateDuringLoading %d",dwRelative);
		else 
			m_pSystem->GetILog()->Log("UpdateDuringLoading %d",dwRelative);

		dwTime=dwNewTime;
	}
*/

	if(m_pServer)
		m_pServer->UpdateXServerNetwork();
}



//////////////////////////////////////////////////////////////////////////
bool CXGame::ParseLevelName(const char *szMsg,char *szLevelName,char *szMissionName)
{	
	const char *szPtr=szMsg;
	char *szDest;

	// get level & mission-name
	szPtr=szMsg+11; //skip "StartLevel "
	//find the level name
	memset(szLevelName,0,32);
	szDest=szLevelName;
	while ((*szPtr) && (*szPtr!=' '))		
		*szDest++=*szPtr++;		

	//find the mission name
	memset(szMissionName,0,32);

	if (*szPtr) //if not eos
	{
		szPtr++; //skip space
		szDest=szMissionName;			
		while (*szPtr)		
			*szDest++=*szPtr++;					
	}

	return (true);
}

//////////////////////////////////////////////////////////////////////
//Process the process messages
//it is needed for example to perform actions that must be done
//the next frame
void CXGame::ProcessPMessages(const char *szMsg)
{
	if (!szMsg) 
		return;

	if ((stricmp(szMsg,"EndDemo") == 0) || (stricmp(szMsg,"EndDemoQuit") == 0))	// used for demos (e3, magazine demos)
	{ 
		ITexPic * pPic = m_pRenderer->EF_LoadTexture("Textures/end_screen.dds", FT_NORESIZE, 0, eTT_Base);
		int img = -1;

		if (pPic)
		{
			img = pPic->GetTextureID();
		}

		GetSystem()->GetIRenderer()->BeginFrame();
		//GetSystem()->GetIRenderer()->Draw2dImage(0,0,(float)(GetSystem()->GetIRenderer()->GetWidth()),(float)(GetSystem()->GetIRenderer()->GetHeight()),img,0,1.0f,1.0f,0);
		// hack! hardcoded to fix the incorrect d3d9 renderer matrix
		GetSystem()->GetIRenderer()->SetState(GS_NODEPTHTEST);
		GetSystem()->GetIRenderer()->Draw2dImage(0,0,(float)(800),(float)(600),img,0,1.0f,1.0f,0);
		GetSystem()->GetIRenderer()->Update();
		float	time = GetSystem()->GetITimer()->GetCurrTime();
		// wait a bit and then quit the game
		while((GetSystem()->GetITimer()->GetCurrTime()-time) < 7)
		{
			GetSystem()->GetITimer()->Update();
		}

		if (stricmp(szMsg,"EndDemoQuit") == 0)
		{
			m_bUpdateRet = false;
		}
		else
		{
			this->SendMessage("Switch");
		}
		return;
	}
	else
	if (stricmp(szMsg,"Quit-Yes") == 0)	// quit message
	{
		m_bUpdateRet = false;
		return;
	}
	else
	if (stricmp(szMsg,"Relaunch") == 0)	// relaunch message
	{
		m_bRelaunch = true;
		m_bUpdateRet = false;
		return;
	}
	else
	if (stricmp(szMsg,"Switch")==0)	// switch process (menu <> game)
	{
		//clear the current message
//		::OutputDebugString("###############switch#############\n");
		//m_pSystem->GetIProcess()->SetPMessage("");				

    if(m_bEditor) return; // we are probably in the editor?

		if (m_bMenuOverlay)				// we're in menu-mode; switch to game
		{
			//check if a game is currently running before switching to
			//the 3d-engine

			CPlayer *pPlayer = 0;
			if (GetMyPlayer())
			{
				GetMyPlayer()->GetContainer()->QueryContainerInterface(CIT_IPLAYER,(void **)&pPlayer);
			}
			// there must be a client for us to be able to go out of the menu
			// if this is a singleplayer game, and the player is alive, we can go back to game
			// otherwise, if this is not multiplayer game and the player is dead, it is locked in the menu
			// also, the client must not be loading, or waiting to connect
			if (m_pClient && m_pClient->IsConnected() && m_pUISystem->GetScriptObjectUI()->CanSwitch(0))
			{
				if (IsMultiplayer() || (!pPlayer || pPlayer->m_stats.health > 0))
				{
					MenuOff();
				}
			}
		}	
		else if (m_pUISystem->GetScriptObjectUI()->CanSwitch(1))
		{
//			::OutputDebugString("->menu\n");
      // quit if menu disabled (usefull during development)
			ICVar * pCvarNoMenu = m_pSystem->GetIConsole()->CreateVariable("g_NoMenu","0",0);

      if(pCvarNoMenu && pCvarNoMenu->GetIVal())
			{
				m_pSystem->Quit();

				return;
			}

			MenuOn();
		}			
	}
	else if (stricmp(szMsg, "EndCutScene")==0)
	{
		if (m_bMenuOverlay)				// we're in menu-mode; switch to game
		{
			if (m_pClient)
			{
				MenuOff();
			}
			else if(!IsMultiplayer())
			{
				// there's no game, so lets quit...
				// exit from game-must be prompted with "Are you Sure?"
				m_pSystem->Quit();
			}
		}	
	}
	else if (stricmp(szMsg,"LoadLastCheckPoint")==0)
	{		
		// the loading funciotn calls game->update 20 times(??)
		// and the script menu system allows to click the reload
		// button again during game->update, so be sure that the game isn't
		// loading already
		if (!m_bIsLoadingLevelFromFile) 
		{	
			LoadLatest();
		}
	}
	else if (strnicmp(szMsg,"StartLevelFade",14)==0)		 // start a level with fade out
	{
		if (!m_bEditor)
		{
			ICVar *g_LevelStated = GetISystem()->GetIConsole()->GetCVar("g_LevelStated");

			if (g_LevelStated && g_LevelStated->GetIVal() && m_pClient && m_pClient->IsConnected())
			{
				HSCRIPTFUNCTION pfnDisplayLevelStats = m_pScriptSystem->GetFunctionPtr("Game", "DisplayTimeZone");

				if (pfnDisplayLevelStats)
				{
					m_pScriptSystem->BeginCall(pfnDisplayLevelStats);
					m_pScriptSystem->PushFuncParam(GetScriptObject());
					m_pScriptSystem->EndCall();

					m_pScriptSystem->ReleaseFunc(pfnDisplayLevelStats);
				}
			}
			else
			{
				m_p3DEngine->ResetScreenFx();
				m_p3DEngine->SetScreenFx("ScreenFade",1);
				float fFadeTime=1.5f;
				m_p3DEngine->SetScreenFxParam("ScreenFade","ScreenFadeTime", &fFadeTime);
				m_fFadingStartTime=m_pSystem->GetITimer()->GetCurrTime();
				AllowQuicksave(false);
				sprintf(m_szLoadMsg,"StartLevel%s",szMsg+14); // skip levelfade
				// disable input handling during fading to load
				m_pSystem->GetIInput()->EnableEventPosting(0);
			}
		}
		else
			m_pSystem->GetILog()->Log("Skipping Load Level in editor (%s)",szMsg);		
	}
	else if (strnicmp(szMsg,"StartLevel",10)==0)		 // start a level
	{
		if (!m_bEditor)
		{
			ICVar *g_LevelStated = GetISystem()->GetIConsole()->GetCVar("g_LevelStated");

			if (g_LevelStated && g_LevelStated->GetIVal() && m_pClient && m_pClient->IsConnected())
			{
				HSCRIPTFUNCTION pfnDisplayLevelStats = m_pScriptSystem->GetFunctionPtr("Game", "DisplayTimeZone");

				if (pfnDisplayLevelStats)
				{
					m_pScriptSystem->BeginCall(pfnDisplayLevelStats);
					m_pScriptSystem->PushFuncParam(GetScriptObject());
					m_pScriptSystem->EndCall();

					m_pScriptSystem->ReleaseFunc(pfnDisplayLevelStats);
				}
			}
			else
			{
				char szLevelName[32];
				char szMissionName[32];

				m_p3DEngine->ResetScreenFx();
				// always disable cryvision at start
				m_p3DEngine->SetScreenFx("NightVision",0);

				// reset fading in case someone will load a level
				// while the current one is fading out
				m_fFadingStartTime=-1; 
				m_p3DEngine->SetScreenFx("ScreenFade",0);

				ParseLevelName(szMsg,szLevelName,szMissionName);

				bool listen = false;
				if (strcmp(szMissionName, "listen")==0)
				{
					szMissionName[0] = 0;

					//strcpy( szMissionName,"" );
					listen = true;
				};

				// disable input handling during load
				m_pSystem->GetIInput()->EnableEventPosting(0);
				m_pSystem->GetIInput()->GetIKeyboard()->ClearKeyState();
				LoadLevelCS(false, szLevelName, szMissionName, listen);
				// finished loading, reenable input handling			
				m_pSystem->GetIInput()->EnableEventPosting(1);
				m_pSystem->GetIInput()->GetIKeyboard()->ClearKeyState();
			}
		}
		else		
			m_pSystem->GetILog()->Log("Skipping Load Level in editor (%s)",szMsg);		
	}			
	else
	if (strnicmp(szMsg,"SaveGame", 8)==0)		// save current game
	{
		if(!m_bEditor)
		{
			//clear the current message
//			m_pSystem->GetIProcess()->SetPMessage("");				
			const char *sname="quicksave";
			if(strlen(szMsg)>8) { 
				sname=szMsg+9;
			}
			Save(sname, NULL, NULL);			
		}
	}
	else
	if (strnicmp(szMsg,"LoadGame", 8)==0)		// load game
	{
		if(!m_bEditor)
		{
      m_p3DEngine->ResetScreenFx();
			//clear the current message
//			m_pSystem->GetIProcess()->SetPMessage("");				
			const char *sname="quicksave";
			if(strlen(szMsg)>8)
			{
				sname=szMsg+9;
			}

			// disable input handling during load
			m_pSystem->GetIInput()->EnableEventPosting(0);
			//m_pSystem->GetIInput()->GetIKeyboard()->ClearKeyState();
			// get out of all the areas localplayer is in - on load it will be retrigererd
			IEntity *pIMyPlayer = GetMyPlayer();
			if( pIMyPlayer )
			{
				IEntityContainer *pIContainer = pIMyPlayer->GetContainer();
				if (pIContainer)
				{
					CPlayer *pPlayer = NULL;
					if (pIContainer->QueryContainerInterface(CIT_IPLAYER, (void**)&pPlayer))
						m_XAreaMgr.ExitAllAreas( pPlayer->m_AreaUser );
				}
			}

			if (Load(sname))
			{
				//if loaded successfull, switch to 3d engine 			
				if(m_pClient)
				{
					m_pSystem->SetIProcess(m_p3DEngine);
					m_pSystem->GetIProcess()->SetFlags(PROC_3DENGINE);								
				}
			}

			// finished loading, reenable input
			m_pSystem->GetIInput()->EnableEventPosting(1);
			//m_pSystem->GetIInput()->GetIKeyboard()->ClearKeyState();
		}
	}
// 	else
//	if (stricmp(szMsg,"ConnectTo")==0)
//	{
//	}
}

//////////////////////////////////////////////////////////////////////////
/*! Load a level on the local machine (connecting with a local client)
	@param dedicated if true the local client will not be created
	@param keepclient if true the current client server connection will be kept
	@param szMapName name of the level that has to be loaded
	@param szMissionName name of the mission that has to be loaded
	@param listen allow external clients to connect
*/
//////////////////////////////////////////////////////////////////////////
void CXGame::LoadLevelCS(bool keepclient, const char *szMapName, const char *szMissionName, bool listen)
{
	// need to reset timers as well
	m_pScriptTimerMgr->Reset();

	if (m_pUISystem)
	{
		m_pUISystem->GetScriptObjectUI()->OnSwitch(0);
		m_pUISystem->StopAllVideo();
		m_p3DEngine->Enable(1);

		m_pSystem->GetILog()->Log("UISystem: Enabled 3D Engine!");
	}
	if (m_pSystem->GetIMovieSystem())
		m_pSystem->GetIMovieSystem()->StopAllCutScenes();
	//m_lstPlayedCutScenes.clear();
	bool bDedicated=GetSystem()->IsDedicated();

	string strGameType = g_GameType->GetString();

	AutoSuspendTimeQuota AutoSuspender(GetSystem()->GetStreamEngine());

	assert( szMissionName != 0 );
	
	string sLevelFolder = szMapName;
	if (sLevelFolder.find('\\') == string::npos && sLevelFolder.find('/') == string::npos)
	{
		// This is just a map name, not a folder.
		sLevelFolder = GetLevelsFolder() + "/" + sLevelFolder;
	}

	IConsole *pConsole=GetSystem()->GetIConsole();
	IInput *pInput=GetSystem()->GetIInput();					// might be 0 (e.g. dedicated server)

	if(pInput)
		pInput->SetMouseExclusive(false);
	
	//if(!*szMissionName) szMissionName = "Multiplayer";

	if (!IsMultiplayer())
	{
		m_pSystem->GetIConsole()->Clear();
		m_pSystem->GetIConsole()->SetScrollMax(600);
		m_pSystem->GetIConsole()->ShowConsole(true);

		string sLoadingScreenTexture = string("levels/") + string(szMapName) + string("/loadscreen_") + string(szMapName) + ".dds";

		m_pSystem->GetIConsole()->SetLoadingImage(sLoadingScreenTexture.c_str());
		m_pSystem->GetIConsole()->ResetProgressBar(0x7fffffff);
		m_pSystem->GetILog()->UpdateLoadingScreen("");	// just to draw the console
	}

	if (m_pClient && !keepclient)
	{
		ShutdownClient();
	}

	// start server
	if((!m_pServer || !keepclient) && !StartupServer(listen))
	{
		m_pLog->LogToConsole("Unable to load the level %s,%s [startup server failed]", sLevelFolder.c_str(),szMissionName);
		if(pInput)
			pInput->SetMouseExclusive(true);
		LoadingError("@LoadLevelError");
		return;
	}

	bool bNeedClient = !bDedicated && ((keepclient && !m_pClient) || !keepclient);

	//create local client(must be before the level is loaded)
	if(bNeedClient)
	{
		if(!StartupLocalClient())
		{
			m_pLog->LogToConsole("Unable to load the level %s,mission %s [startup client failed]", sLevelFolder.c_str(),szMissionName);
			if(pInput)
				pInput->SetMouseExclusive(true);
			LoadingError("@LoadLevelError");
			return;
		}
	}

	const char *szMission = szMissionName;
	if (!*szMissionName)
		szMission=strGameType.c_str();

	// KIRILL lets reset - they will be spawned anyway
	m_pSystem->GetAISystem()->Reset();
	
	// refresh the current server info for incoming queries during loading
	m_pServer->GetServerInfo();

	// load the level
	if(!m_pServer->m_pISystem->LoadLevel( sLevelFolder.c_str(),szMission,false))
	{
		m_pLog->LogToConsole("Unable to load the level %s,mission %s \n", sLevelFolder.c_str(),szMissionName);
		if (pInput)
			pInput->SetMouseExclusive(true);
		LoadingError("@LoadLevelError");
		return;
	}

// start and connect a local client
	if(bNeedClient)
	{
		if(m_pClient)
		{
			if (IsMultiplayer() && m_pServer->m_pIServer->GetServerType()!=eMPST_LAN)
     		m_pClient->XConnect("127.0.0.1",false,true);
			else
				m_pClient->XConnect("127.0.0.1");
		}
	}
	
	if(m_pClient)
    m_pClient->OnMapChanged();
	if(m_pServer)
    m_pServer->OnMapChanged(); 
	if(m_pClient) m_pClient->ForceConnected();
	m_bMenuOverlay = false;
	if (m_pUIHud) { m_pUIHud->Reset(); SetCurrentUI(m_pUIHud); }
	m_p3DEngine->Enable(1);
	m_pSystem->SetIProcess(m_p3DEngine);
	m_pSystem->GetIConsole()->ShowConsole(false);
	if(pInput)
		pInput->SetMouseExclusive(true);
	AllowQuicksave(true);
};

//////////////////////////////////////////////////////////////////////////
bool CXGame::GetLevelMissions( const char *szLevelDir,std::vector<string> &missions )
{
	string sLevelPath = szLevelDir;
	
	if (!szLevelDir || sLevelPath.empty())	
		return false;

	string sEPath = sLevelPath+string("/levelinfo.xml");
	string sPaks = sLevelPath + "/*.pak";
	//GetSystem()->GetIPak()->OpenPacks(sPaks.c_str());	
	OpenPacks(sPaks.c_str());

	XmlNodeRef root = GetSystem()->LoadXmlFile( sEPath.c_str() );
	bool bResult = false;
	if (root)
	{
		XmlNodeRef missionsNode = root->findChild( "Missions" );
		if (missionsNode)
		{
			// we found a mission node - the level is valid
			bResult = true;
			for (int i = 0; i < missionsNode->getChildCount(); i++)
			{
				XmlNodeRef missionNode = missionsNode->getChild(i);
				if (missionNode->isTag( "Mission" ))
				{
					const char *sMissionName = missionNode->getAttr( "Name" );
					if (sMissionName)
						missions.push_back( sMissionName );
				}
			}
		}
	}
	//GetSystem()->GetIPak()->
	ClosePacks(sPaks.c_str());
	return bResult;
}

//////////////////////////////////////////////////////////////////////////
bool CXGame::IsMultiplayer()
{
	// cannot be in multiplayer when in editor
	if (m_bEditor)
		return false;

	bool bServer=IsServer();
	bool bClient=IsClient();

	if(!bServer && !bClient)
		return false;

	return !bServer || !bClient || m_pServer->m_bListen;
};

//////////////////////////////////////////////////////////////////////////
void CXGame::ResetState(void)
{
//	m_PlayersWithLighs.clear();
	m_pSystem->GetIEntitySystem()->ResetEntities();
	if (GetMyPlayer())
	{
		m_XAreaMgr.ReTriggerArea(GetMyPlayer(), GetMyPlayer()->GetPos(),false);
		GetMyPlayer()->SetNeedUpdate(true);
	}

	m_pSystem->GetIInput()->Update(1);	// flush the keyboard buffers
	m_pSystem->GetIInput()->Update(1);	// flush the keyboard buffers
	m_pSystem->GetIInput()->GetIKeyboard()->ClearKeyState();
}

//////////////////////////////////////////////////////////////////////////
bool CXGame::ConstrainToSandbox(IEntity *pEntity)
{
	bool bPosChanged = false;
	if (IsMultiplayer())
	{
		Vec3 vPos, vBounds[2]={Vec3(0,0,0),Vec3(0,0,0)};
		IPhysicalEntity *pPhysEnt = pEntity->GetPhysics();
		if (pPhysEnt)
		{
			pe_status_pos sp;
			pPhysEnt->GetStatus(&sp);
			vPos = sp.pos;
		}
		else
			vPos=pEntity->GetPos();
		
		vBounds[1].x = vBounds[1].y = (float)m_p3DEngine->GetTerrainSize();
		vBounds[0].z = -100; vBounds[1].z = 500;
		for(int i=0;i<3;i++) 
			if (!inrange(vPos[i], vBounds[0][i],vBounds[1][i]))
			{
				vPos[i] = vBounds[isneg((vBounds[0][i]+vBounds[1][i])*0.5f-vPos[i])][i];
				bPosChanged = true;
			}

		if (bPosChanged)
		{
			if (pPhysEnt)
			{
				pe_params_pos pp;
				pp.pos = vPos;
				pPhysEnt->SetParams(&pp);
			}
			else
				pEntity->SetPos(vPos);
		}
	}
	return bPosChanged;
}

//////////////////////////////////////////////////////////////////////////
void CXGame::GotoMenu(bool bTriggerOnSwitch)
{
	if(m_bEditor)
		return;

	if(!m_pUISystem)			// e.g. dedicated server
		return;

	if (!IsInMenu())
	{
		DeleteMessage("Switch");
		SendMessage("Switch");
	}
	else if (bTriggerOnSwitch)
	{
		m_pUISystem->GetScriptObjectUI()->OnSwitch(1);
	}
}

//////////////////////////////////////////////////////////////////////////
void CXGame::GotoGame(bool bTriggerOnSwitch)
{
	if (m_bEditor)
	{
		return;
	}

	if (IsInMenu())
	{
		DeleteMessage("Switch");
		SendMessage("Switch");
	}
	else if (bTriggerOnSwitch)
	{
		m_pUISystem->GetScriptObjectUI()->OnSwitch(0);
	}
}

//////////////////////////////////////////////////////////////////////////
void CXGame::MenuOn()
{
	// stop sounds and timers affected by game pause
	m_pSystem->GetISoundSystem()->Pause(true,true); 
	m_pScriptTimerMgr->Pause(true);

	if (m_pSystem->GetIMusicSystem())
	{
		m_pSystem->GetIMusicSystem()->Pause(true);
		//m_pSystem->GetIMusicSystem()->Silence();
	}

	if (m_pUISystem && m_pUISystem->IsEnabled())
	{
		m_pSystem->GetIInput()->AddEventListener(m_pUISystem);
		m_pSystem->GetIInput()->ClearKeyState();
		m_pUISystem->GetScriptObjectUI()->OnSwitch(1);
	}

	m_bMenuOverlay = 1;

	if (!m_bEditor)
	{
		if (m_pUISystem->GetScriptObjectUI()->CanRenderGame())
		{
			m_pSystem->GetILog()->Log("UISystem: Enabled 3D Engine!");
			m_p3DEngine->Enable(1);
		}
		else
		{
			m_pSystem->GetILog()->Log("UISystem: Disabled 3D Engine!");
			m_p3DEngine->Enable(0);
		}
	}
	
	_SmartScriptObject pClientStuff(m_pScriptSystem, true);

	if(m_pScriptSystem->GetGlobalValue("ClientStuff",pClientStuff))
	{
		m_pScriptSystem->BeginCall("ClientStuff","OnPauseGame");
		m_pScriptSystem->PushFuncParam(pClientStuff);
		m_pScriptSystem->EndCall();
	}
}

//////////////////////////////////////////////////////////////////////////
void CXGame::MenuOff()
{
	// resume sounds and timers affected by game pause
	m_pSystem->GetISoundSystem()->Pause(false);
	m_pSystem->GetIMusicSystem()->Pause(false);
	m_pScriptTimerMgr->Pause(false);


	if (m_pUISystem && m_pUISystem->IsEnabled())
	{
		m_pSystem->GetIInput()->RemoveEventListener(m_pUISystem);
		m_pSystem->GetIInput()->ClearKeyState();
		m_pUISystem->GetScriptObjectUI()->OnSwitch(0);

		if (GetMyPlayer())
			m_XAreaMgr.ReTriggerArea(GetMyPlayer(), GetMyPlayer()->GetPos(),false);
		//					m_XAreaMgr.ReTriggerArea(GetMyPlayer(), m_pSystem->GetISoundSystem()->GetListenerPos(),false);
	}

	m_bMenuOverlay = 0;

	m_pSystem->SetIProcess(m_p3DEngine);
	m_pSystem->GetIProcess()->SetFlags(PROC_3DENGINE);

	if (!m_bEditor)
	{
		m_pSystem->GetILog()->Log("UISystem: Enabled 3D Engine!");
		m_p3DEngine->Enable(1);
	}

	_SmartScriptObject pClientStuff(m_pScriptSystem, true);

	if(m_pScriptSystem->GetGlobalValue("ClientStuff",pClientStuff))
	{
		m_pScriptSystem->BeginCall("ClientStuff","OnResumeGame");
		m_pScriptSystem->PushFuncParam(pClientStuff);
		m_pScriptSystem->EndCall();
	}
}

//////////////////////////////////////////////////////////////////////////
void CXGame::DeleteMessage(const char *szMessage)
{
	StringQueue NewQueue;

	while(!m_qMessages.empty())
	{
		string qMessage = m_qMessages.front();

		m_qMessages.pop();

		if (qMessage != szMessage)
		{
			NewQueue.push(qMessage);
		}
	}

	m_qMessages = NewQueue;
}

//////////////////////////////////////////////////////////////////////////
IScriptObject *CXGame::GetScriptObject()
{
	if (!m_pScriptObjectGame)
	{
		return 0;
	}
	
	return m_pScriptObjectGame->GetScriptObject();
}


void CXGame::PlaySubtitle(ISound * pSound)
{
	if (m_pMovieUser)
		m_pMovieUser->PlaySubtitles(pSound);
}

vector2f CXGame::GetSubtitleSize(const string &szMessage, float sizex, float sizey, const string &szFontName, const string &szFontEffect)
{
	IFFont *pFont = m_pSystem->GetICryFont()->GetFont(szFontName.c_str());

	pFont->Reset();
	pFont->SetEffect(szFontEffect.c_str());
	pFont->SetSize(vector2f(sizex, sizey));

	wstring szwString;

	m_StringTableMgr.Localize(szMessage, szwString);

	return pFont->GetTextSizeW(szwString.c_str());
}

void CXGame::WriteSubtitle(const string &szMessage, float x, float y, float sizex, float sizey, const color4f &cColor, const string &szFontName, const string &szFontEffect)
{
	IFFont *pFont = m_pSystem->GetICryFont()->GetFont(szFontName.c_str());

	pFont->Reset();
	pFont->SetEffect(szFontEffect.c_str());
	pFont->SetSize(vector2f(sizex, sizey));
	pFont->SetColor(cColor);

	wstring szwString;

	m_StringTableMgr.Localize(szMessage, szwString);

	pFont->DrawStringW(x, y, szwString.c_str());
}

//////////////////////////////////////////////////////////////////////////
IGameMods* CXGame::GetModsInterface()
{
	return m_pGameMods;	
};

//////////////////////////////////////////////////////////////////////////
void CXGame::LoadingError(const char *szError)
{
	m_pRenderer->ClearColorBuffer(Vec3(0,0,0));
	GetSystem()->GetIConsole()->ResetProgressBar(0);
	m_pSystem->GetIConsole()->ShowConsole(false);
	m_pSystem->GetIConsole()->SetScrollMax(600/2);

	m_pScriptSystem->BeginCall("Game", "OnLoadingError");
	m_pScriptSystem->PushFuncParam(GetScriptObject());
	m_pScriptSystem->PushFuncParam(szError);
	m_pScriptSystem->EndCall();
}
//------------------------------------------------------------------------------------------------- 
bool CXGame::GetCDPath(string &szCDPath)
{
	bool bRet( false );
#ifdef WIN32
	DWORD nBufferSize( GetLogicalDriveStrings( 0, 0 ) );
	if( 0 < nBufferSize )
	{
		// get list of all available logical drives
		tvector rawDriveLetters( nBufferSize + 1 );
		GetLogicalDriveStrings( nBufferSize, &rawDriveLetters[ 0 ] );

		// quickly scan all drives
		tvector::size_type i( 0 );
		while( true )
		{
			// check if current drive is cd/dvd drive
			if( DRIVE_CDROM == GetDriveType( &rawDriveLetters[ i ] ) )
			{
				// get volume name
				tvector cdVolumeName( MAX_VOLUME_ID_SIZE + 1 );
				if( FALSE != GetVolumeInformation( &rawDriveLetters[ i ],
					&cdVolumeName[ 0 ], (DWORD) cdVolumeName.size(), 0, 0, 0, 0, 0 ) )
				{
					// check volume name to verify it's Far Cry's game cd/dvd
					tstring cdVolumeLabel( &cdVolumeName[ 0 ] );
					tstring farCryDisk1Label( _T( "FARCRY_1" ) );
					if( cdVolumeLabel == farCryDisk1Label  )
					{
						// found Far Cry's game cd/dvd, copy information and bail out
						szCDPath = &rawDriveLetters[ i ];
						bRet = true;
						break;
					}
				}
			}

			// proceed to next drive
			while( 0 != rawDriveLetters[ i ] )
			{
				++i;
			}
			++i; // skip null termination of current drive

			// check if we're out of drive letters
			if( 0 == rawDriveLetters[ i ] )
			{
				// double null termination found, bail out
				break;
			}
		}
	}
#endif
	return( bRet );
}

ITagPointManager* CXGame::GetTagPointManager()
{
	return m_pTagPointManager;
}
