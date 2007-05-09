#include "precompiled.h"

#include "lib/external_libraries/sdl.h"
#include "lib/ogl.h"
#include "lib/timer.h"
#include "lib/input.h"
#include "lib/app_hooks.h"
#include "lib/sysdep/cpu.h"
#include "lib/sysdep/gfx.h"
#include "lib/res/res.h"
#include "lib/res/file/trace.h"
#include "lib/res/sound/snd_mgr.h"
#include "lib/res/graphics/tex.h"
#include "lib/res/graphics/cursor.h"

#include "ps/CConsole.h"
#include "ps/CLogger.h"
#include "ps/ConfigDB.h"
#include "ps/Font.h"
#include "ps/Game.h"
#include "ps/Globals.h"
#include "ps/Hotkey.h"
#include "ps/Interact.h"
#include "ps/Loader.h"
#include "ps/Overlay.h"
#include "ps/Profile.h"
#include "ps/ProfileViewer.h"
#include "ps/StringConvert.h"
#include "ps/Util.h"
#include "ps/i18n.h"

#include "graphics/CinemaTrack.h"
#include "graphics/GameView.h"
#include "graphics/LightEnv.h"
#include "graphics/MapReader.h"
#include "graphics/MaterialManager.h"
#include "graphics/ParticleEngine.h"
#include "graphics/TextureManager.h"

#include "renderer/Renderer.h"
#include "renderer/VertexBufferManager.h"

#include "maths/MathUtil.h"

#include "simulation/Entity.h"
#include "simulation/EntityHandles.h"
#include "simulation/EntityManager.h"
#include "simulation/EntityTemplate.h"
#include "simulation/EntityTemplateCollection.h"
#include "simulation/EventHandlers.h"
#include "simulation/FormationCollection.h"
#include "simulation/FormationManager.h"
#include "simulation/TerritoryManager.h"
#include "simulation/TriggerManager.h"
#include "simulation/PathfindEngine.h"
#include "simulation/Projectile.h"
#include "simulation/Scheduler.h"
#include "simulation/TechnologyCollection.h"

#include "scripting/ScriptableComplex.inl"
#include "scripting/ScriptingHost.h"
#include "scripting/GameEvents.h"
#include "scripting/ScriptableComplex.h"
#include "maths/scripting/JSInterface_Vector3D.h"
#include "graphics/scripting/JSInterface_Camera.h"
#include "ps/scripting/JSInterface_Selection.h"
#include "ps/scripting/JSInterface_Console.h"
#include "graphics/scripting/JSInterface_LightEnv.h"
#include "ps/scripting/JSCollection.h"
#include "scripting/DOMEvent.h"
#ifndef NO_GUI
# include "gui/scripting/JSInterface_IGUIObject.h"
# include "gui/scripting/JSInterface_GUITypes.h"
# include "gui/GUI.h"
#endif

#include "sound/CMusicPlayer.h"
#include "sound/JSI_Sound.h"

#include "network/SessionManager.h"
#include "network/Server.h"
#include "network/Client.h"

#include "ps/GameSetup/Atlas.h"
#include "ps/GameSetup/GameSetup.h"
#include "ps/GameSetup/Config.h"
#include "ps/GameSetup/CmdLineArgs.h"

ERROR_GROUP(System);
ERROR_TYPE(System, SDLInitFailed);
ERROR_TYPE(System, VmodeFailed);
ERROR_TYPE(System, RequiredExtensionsMissing);

#define LOG_CATEGORY "gamesetup"





static int SetVideoMode(int w, int h, int bpp, bool fullscreen)
{
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	ulong flags = SDL_OPENGL;
	if(fullscreen)
		flags |= SDL_FULLSCREEN;
	if(!SDL_SetVideoMode(w, h, bpp, flags))
		return -1;

	// Work around a bug in the proprietary Linux ATI driver (at least versions 8.16.20 and 8.14.13).
	// The driver appears to register its own atexit hook on context creation.
	// If this atexit hook is called before SDL_Quit destroys the OpenGL context,
	// some kind of double-free problem causes a crash and lockup in the driver.
	// Calling SDL_Quit twice appears to be harmless, though, and avoids the problem
	// by destroying the context *before* the driver's atexit hook is called.
	// (Note that atexit hooks are guarantueed to be called in reverse order of their registration.)
	atexit(SDL_Quit);
	// End work around.

	glViewport(0, 0, w, h);

#ifndef NO_GUI
	g_GUI.UpdateResolution();
#endif

	ogl_Init();	// required after each mode change

	if(SDL_SetGamma(g_Gamma, g_Gamma, g_Gamma) < 0)
		debug_warn("SDL_SetGamma failed");

	return 0;
}

static const uint SANE_TEX_QUALITY_DEFAULT = 5;	// keep in sync with code

static void SetTextureQuality(uint quality)
{
	uint q_flags;
	GLint filter;

retry:
	// keep this in sync with SANE_TEX_QUALITY_DEFAULT
	switch(quality)
	{
		// worst quality
	case 0:
		q_flags = OGL_TEX_HALF_RES|OGL_TEX_HALF_BPP;
		filter = GL_NEAREST;
		break;
		// [perf] add bilinear filtering
	case 1:
		q_flags = OGL_TEX_HALF_RES|OGL_TEX_HALF_BPP;
		filter = GL_LINEAR;
		break;
		// [vmem] no longer reduce resolution
	case 2:
		q_flags = OGL_TEX_HALF_BPP;
		filter = GL_LINEAR;
		break;
		// [vmem] add mipmaps
	case 3:
		q_flags = OGL_TEX_HALF_BPP;
		filter = GL_NEAREST_MIPMAP_LINEAR;
		break;
		// [perf] better filtering
	case 4:
		q_flags = OGL_TEX_HALF_BPP;
		filter = GL_LINEAR_MIPMAP_LINEAR;
		break;
		// [vmem] no longer reduce bpp
	case SANE_TEX_QUALITY_DEFAULT:
		q_flags = OGL_TEX_FULL_QUALITY;
		filter = GL_LINEAR_MIPMAP_LINEAR;
		break;
		// [perf] add anisotropy
	case 6:
		// TODO: add anisotropic filtering
		q_flags = OGL_TEX_FULL_QUALITY;
		filter = GL_LINEAR_MIPMAP_LINEAR;
		break;
		// invalid
	default:
		debug_warn("SetTextureQuality: invalid quality");
		quality = SANE_TEX_QUALITY_DEFAULT;
		// careful: recursion doesn't work and we don't want to duplicate
		// the "sane" default values.
		goto retry;
	}

	ogl_tex_set_defaults(q_flags, filter);
}


//----------------------------------------------------------------------------
// GUI integration
//----------------------------------------------------------------------------

void GUI_Init()
{
#ifndef NO_GUI
	{TIMER("ps_gui_init");
	g_GUI.Initialize();}

	{TIMER("ps_gui_setup_xml");
	g_GUI.LoadXmlFile("gui/test/setup.xml");}
	{TIMER("ps_gui_styles_xml");
	g_GUI.LoadXmlFile("gui/test/styles.xml");}
	{TIMER("ps_gui_sprite1_xml");
	g_GUI.LoadXmlFile("gui/test/sprite1.xml");}

	// Atlas is running, we won't need these GUI pages (for now!
	// what if Atlas switches to in-game mode?!)
	// TODO: temporary hack until revised GUI structure is completed.
//	if(ATLAS_IsRunning())
//		return;

	{TIMER("ps_gui_1");
	g_GUI.LoadXmlFile("gui/test/1_init.xml");}
	{TIMER("ps_gui_2");
	g_GUI.LoadXmlFile("gui/test/2_mainmenu.xml");}
	{TIMER("ps_gui_3");
	g_GUI.LoadXmlFile("gui/test/3_loading.xml");}
	{TIMER("ps_gui_4");
	g_GUI.LoadXmlFile("gui/test/4_session.xml");}
	{TIMER("ps_gui_6");
	g_GUI.LoadXmlFile("gui/test/6_subwindows.xml");}
	{TIMER("ps_gui_6_1");
	g_GUI.LoadXmlFile("gui/test/6_1_manual.xml");}
	{TIMER("ps_gui_6_2");
	g_GUI.LoadXmlFile("gui/test/6_2_jukebox.xml");}
	{TIMER("ps_gui_7");
	g_GUI.LoadXmlFile("gui/test/7_atlas.xml");}
	{TIMER("ps_gui_9");
	g_GUI.LoadXmlFile("gui/test/9_global.xml");}
#endif
}


void GUI_Shutdown()
{
#ifndef NO_GUI
	g_GUI.Destroy();
	delete &g_GUI;
#endif
}


void GUI_ShowMainMenu()
{

}


// display progress / description in loading screen
void GUI_DisplayLoadProgress(int percent, const wchar_t* pending_task)
{
#ifndef NO_GUI
	CStrW i18n_description = I18n::translate(pending_task);
	JSString* js_desc = StringConvert::wstring_to_jsstring(g_ScriptingHost.getContext(), i18n_description);
	g_ScriptingHost.SetGlobal("g_Progress", INT_TO_JSVAL(percent));
	g_ScriptingHost.SetGlobal("g_LoadDescription", STRING_TO_JSVAL(js_desc));
	g_GUI.SendEventToAll("progress");
#endif
}



void Render()
{
	MICROLOG(L"begin frame");

	ogl_WarnIfError();

#ifndef NO_GUI // HACK: because colour-parsing requires the GUI
	CStr skystring = "61 193 255";
	CFG_GET_USER_VAL("skycolor", String, skystring);
	CColor skycol;
	GUI<CColor>::ParseString(skystring, skycol);
	g_Renderer.SetClearColor(skycol.Int());
#endif

	// start new frame
	g_Renderer.BeginFrame();

	ogl_WarnIfError();

	if (g_Game && g_Game->IsGameStarted())
	{
		g_Game->GetView()->Render();

		glPushAttrib( GL_ENABLE_BIT );
		glDisable( GL_LIGHTING );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_DEPTH_TEST );

		if( g_EntGraph )
		{
			PROFILE( "render entity overlays" );
			glColor3f( 1.0f, 0.0f, 1.0f );
			g_EntityManager.RenderAll(); // <-- collision outlines, pathing routes
		}

		glEnable( GL_DEPTH_TEST );

		PROFILE_START( "render entity outlines" );
		g_Mouseover.RenderSelectionOutlines();
		g_Selection.RenderSelectionOutlines();
		PROFILE_END( "render entity outlines" );
		
		PROFILE_START( "render entity auras" );
		g_Mouseover.RenderAuras();
		g_Selection.RenderAuras();
		PROFILE_END( "render entity auras" );

		glDisable(GL_DEPTH_TEST);

		PROFILE_START( "render entity bars" );
		pglActiveTextureARB(GL_TEXTURE1_ARB);
		glDisable(GL_TEXTURE_2D);
		pglActiveTextureARB(GL_TEXTURE0_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_ARB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA_ARB, GL_SRC_ALPHA);
		glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, g_Renderer.m_Options.m_LodBias);
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		/*glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glOrtho(0.f, (float)g_xres, 0.f, (float)g_yres, -1.0f, 1000.f);
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();*/
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		g_Mouseover.RenderBars();
		g_Selection.RenderBars();
		
		glDisable(GL_BLEND);
		/*glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);*/
		PROFILE_END( "render entity bars" );

		glPopAttrib();
		glMatrixMode(GL_MODELVIEW);

		// Depth test is now enabled
		PROFILE_START( "render rally points" );
		g_Selection.RenderRallyPoints();
		g_Mouseover.RenderRallyPoints();
		PROFILE_END( "render rally points" );
		
		PROFILE_START( "render cinematic splines" );
		//Sets/resets renderering properties itself
		g_Game->GetView()->GetCinema()->DrawSpline();
		PROFILE_END( "render cinematic splines" );
	}

	ogl_WarnIfError();

	PROFILE_START( "render fonts" );
	MICROLOG(L"render fonts");
	// overlay mode
	glPushAttrib(GL_ENABLE_BIT);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_CULL_FACE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.f, (float)g_xres, 0.f, (float)g_yres, -1.f, 1000.f);


	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	PROFILE_END( "render fonts" );

	ogl_WarnIfError();

#ifndef NO_GUI
	// Temp GUI message GeeTODO
	MICROLOG(L"render GUI");
	PROFILE_START( "render gui" );
	g_GUI.Draw();
	PROFILE_END( "render gui" );
#endif

	ogl_WarnIfError();

	// Particle Engine Updating
	CParticleEngine::GetInstance()->UpdateEmitters();

	// Text:

	// Use the GL_ALPHA texture as the alpha channel with a flat colouring
	glDisable(GL_ALPHA_TEST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	// Added --
	glEnable(GL_TEXTURE_2D);
	// -- GL

	ogl_WarnIfError();

	{
		PROFILE( "render console" );
		glLoadIdentity();

		MICROLOG(L"render console");
		CFont font("console");
		font.Bind();
		g_Console->Render();
	}

	ogl_WarnIfError();


	// Profile information

	PROFILE_START( "render profiling" );
	g_ProfileViewer.RenderProfile();
	PROFILE_END( "render profiling" );

	ogl_WarnIfError();

	if (g_Game && g_Game->IsGameStarted())
	{
		PROFILE( "render selection overlays" );
		g_Mouseover.RenderOverlays();
		g_Selection.RenderOverlays();
	}

	ogl_WarnIfError();

	// Draw the cursor (or set the Windows cursor, on Windows)
	CStr cursorName = g_BuildingPlacer.m_active ? "action-build" : g_CursorName;
	cursor_draw(cursorName, g_mouse_x, g_mouse_y);

	// restore
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glPopAttrib();

	MICROLOG(L"end frame");
	g_Renderer.EndFrame();

	ogl_WarnIfError();
}



static void InitScripting()
{
	TIMER("InitScripting");

	// Create the scripting host.  This needs to be done before the GUI is created.
	// [7ms]
	new ScriptingHost;
	
	// It would be nice for onLoad code to be able to access the setTimeout() calls.
	new CScheduler;

	// Register the JavaScript interfaces with the runtime
	SColour::ScriptingInit();
	CEntity::ScriptingInit();
	CTrigger::ScriptingInit();
	CEntityTemplate::ScriptingInit();

	JSI_Sound::ScriptingInit();
	CProfileNode::ScriptingInit();

#ifndef NO_GUI
	JSI_IGUIObject::init();
	JSI_GUITypes::init();
#endif
	JSI_Vector3D::init();
	EntityCollection::Init( "EntityCollection" );
	CPlayer::ScriptingInit();

	PlayerCollection::Init( "PlayerCollection" );

	// call CJSComplexPropertyAccessor's ScriptingInit. doesn't really
	// matter which <T> we use, but we know CJSPropertyAccessor<T> is
	// already being compiled for T = CEntity.
	ScriptableComplex_InitComplexPropertyAccessor<CEntity>();

	CScriptEvent::ScriptingInit();
	CJSProgressTimer::ScriptingInit();
	CProjectile::ScriptingInit();

	g_ScriptingHost.DefineConstant( "FORMATION_ENTER", CFormationEvent::FORMATION_ENTER );
	g_ScriptingHost.DefineConstant( "FORMATION_LEAVE", CFormationEvent::FORMATION_LEAVE );
	g_ScriptingHost.DefineConstant( "FORMATION_DAMAGE", CFormationEvent::FORMATION_DAMAGE );
	g_ScriptingHost.DefineConstant( "FORMATION_ATTACK", CFormationEvent::FORMATION_ATTACK );

	g_ScriptingHost.DefineConstant( "NOTIFY_NONE", CEntityListener::NOTIFY_NONE );
	g_ScriptingHost.DefineConstant( "NOTIFY_GOTO", CEntityListener::NOTIFY_GOTO );
	g_ScriptingHost.DefineConstant( "NOTIFY_RUN", CEntityListener::NOTIFY_RUN );
	g_ScriptingHost.DefineConstant( "NOTIFY_FOLLOW", CEntityListener::NOTIFY_FOLLOW );
	g_ScriptingHost.DefineConstant( "NOTIFY_ATTACK", CEntityListener::NOTIFY_ATTACK );
	g_ScriptingHost.DefineConstant( "NOTIFY_DAMAGE", CEntityListener::NOTIFY_DAMAGE );
	g_ScriptingHost.DefineConstant( "NOTIFY_COMBAT", CEntityListener::NOTIFY_COMBAT );
	g_ScriptingHost.DefineConstant( "NOTIFY_ESCORT", CEntityListener::NOTIFY_ESCORT );
	g_ScriptingHost.DefineConstant( "NOTIFY_HEAL", CEntityListener::NOTIFY_HEAL );
	g_ScriptingHost.DefineConstant( "NOTIFY_GATHER", CEntityListener::NOTIFY_GATHER );
	g_ScriptingHost.DefineConstant( "NOTIFY_IDLE", CEntityListener::NOTIFY_IDLE );
	g_ScriptingHost.DefineConstant( "NOTIFY_ORDER_CHANGE", CEntityListener::NOTIFY_ORDER_CHANGE );
	g_ScriptingHost.DefineConstant( "NOTIFY_ALL", CEntityListener::NOTIFY_ALL );
	
	g_ScriptingHost.DefineConstant( "ORDER_NONE", -1 );
	g_ScriptingHost.DefineConstant( "ORDER_GOTO", CEntityOrder::ORDER_GOTO );
	g_ScriptingHost.DefineConstant( "ORDER_RUN", CEntityOrder::ORDER_RUN );
	g_ScriptingHost.DefineConstant( "ORDER_PATROL", CEntityOrder::ORDER_PATROL );
	g_ScriptingHost.DefineConstant( "ORDER_GENERIC", CEntityOrder::ORDER_GENERIC );
	g_ScriptingHost.DefineConstant( "ORDER_PRODUCE", CEntityOrder::ORDER_PRODUCE );
	g_ScriptingHost.DefineConstant( "ORDER_START_CONSTRUCTION", CEntityOrder::ORDER_START_CONSTRUCTION );

#define REG_JS_CONSTANT(_name) g_ScriptingHost.DefineConstant(#_name, _name)
	REG_JS_CONSTANT(SDL_BUTTON_LEFT);
	REG_JS_CONSTANT(SDL_BUTTON_MIDDLE);
	REG_JS_CONSTANT(SDL_BUTTON_RIGHT);
	REG_JS_CONSTANT(SDL_BUTTON_WHEELUP);
	REG_JS_CONSTANT(SDL_BUTTON_WHEELDOWN);
#undef REG_JS_CONSTANT

	CNetMessage::ScriptingInit();

	JSI_Camera::init();
	JSI_Console::init();
	JSI_LightEnv::init();

	new CGameEvents;
}


static void InitVfs(const CmdLineArgs& args)
{
	TIMER("InitVfs");

	(void)file_init();

	// set root directory to "$game_dir/data". all relative file paths
	// passed to file.cpp will be based from this dir.
	// (we don't set current directory because other libraries may
	// hijack it).
	//
	// "../data" is relative to the executable (in "$game_dir/system").
	//
	// rationale for data/ being root: untrusted scripts must not be
	// allowed to overwrite critical game (or worse, OS) files.
	// the VFS prevents any accesses to files above this directory.
	(void)file_set_root_dir(args.GetArg0(), "../data");

	vfs_init();

	vfs_mount("screenshots/", "screenshots");
	vfs_mount("profiles/", "profiles", VFS_MOUNT_RECURSIVE);

	// rationale:
	// - this is in a separate real directory so that it can later be moved
	//   to $APPDATA to allow running without Admin access.
	// - we mount as archivable so that all files will be added to archive.
	//   even though we write out XMBs here, they will eventually be read,
	//   so putting them in an archive boosts performance.
	//
	// [hot: 16ms]
	vfs_mount("cache/", "cache", VFS_MOUNT_RECURSIVE|VFS_MOUNT_ARCHIVES|VFS_MOUNT_ARCHIVABLE);

	std::vector<CStr> mods = args.GetMultiple("mod");
	if (mods.empty())
		mods.push_back("official");

	for (size_t i = 0; i < mods.size(); ++i)
	{
		CStr path = "mods/" + mods[i];
		uint priority = (uint)i;
		uint flags = VFS_MOUNT_RECURSIVE|VFS_MOUNT_ARCHIVES|VFS_MOUNT_WATCH;

		// TODO: currently only archive 'official' - probably ought to archive
		// all mods instead?
		if (mods[i] == "official")
			flags |= VFS_MOUNT_ARCHIVABLE;

		// [hot: 150ms for mods/official]
		(void)vfs_mount("", path, flags, priority);
	}

	// set the top (last) mod to be the write target
	CStr top_mod_path = "mods/" + mods.back(); // (mods is never empty)
	vfs_set_write_target(top_mod_path);

	// don't try vfs_display yet: SDL_Init hasn't yet redirected stdout
}


static void InitPs(bool setup_gui)
{
	if (setup_gui)
	{
		// The things here aren't strictly GUI, but they're unnecessary when in Atlas
		// because the game doesn't draw any text or handle keys or anything

		{
			// console
			TIMER("ps_console");

			g_Console->UpdateScreenSize(g_xres, g_yres);

			// Calculate and store the line spacing
			CFont font("console");
			g_Console->m_iFontHeight = font.GetLineSpacing();
			g_Console->m_iFontWidth = font.GetCharacterWidth(L'C');
			g_Console->m_charsPerPage = (size_t)(g_xres / g_Console->m_iFontWidth);
			// Offset by an arbitrary amount, to make it fit more nicely
			g_Console->m_iFontOffset = 9;
		}

		// language and hotkeys
		{
			TIMER("ps_lang_hotkeys");

			std::string lang = "english";
			CFG_GET_SYS_VAL("language", String, lang);
			I18n::LoadLanguage(lang.c_str());

			LoadHotkeys();
		}

		// GUI uses VFS, so this must come after VFS init.
		GUI_Init();
	}
}


static void InitInput()
{
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);

	// register input handlers
	// This stack is constructed so the first added, will be the last
	//  one called. This is important, because each of the handlers
	//  has the potential to block events to go further down
	//  in the chain. I.e. the last one in the list added, is the
	//  only handler that can block all messages before they are
	//  processed.
	in_add_handler(game_view_handler);

	in_add_handler(InteractInputHandler);

	in_add_handler(conInputHandler);

	in_add_handler(CProfileViewer::InputThunk);

	in_add_handler(HotkeyInputHandler);

	in_add_handler(GlobalsInputHandler);
}


static void ShutdownPs()
{
	GUI_Shutdown();

	delete g_Console;
	g_Console = 0;

	// disable the special Windows cursor, or free textures for OGL cursors
	cursor_draw(0, g_mouse_x, g_mouse_y);

	// close down Xerces if it was loaded
	CXeromyces::Terminate();

	// Unload the real language (since it depends on the scripting engine,
	// which is going to be killed later) and use the English fallback messages
	I18n::LoadLanguage(NULL);
}


static void InitRenderer()
{
	TIMER("InitRenderer");
	// create renderer
	new CRenderer;

	// set renderer options from command line options - NOVBO must be set before opening the renderer
	g_Renderer.SetOptionBool(CRenderer::OPT_NOVBO,g_NoGLVBO);
	g_Renderer.SetOptionBool(CRenderer::OPT_NOFRAMEBUFFEROBJECT,g_NoGLFramebufferObject);
	g_Renderer.SetOptionBool(CRenderer::OPT_SHADOWS,g_Shadows);
	g_Renderer.SetOptionBool(CRenderer::OPT_FANCYWATER,g_FancyWater);
	g_Renderer.SetRenderPath(CRenderer::GetRenderPathByName(g_RenderPath));
	g_Renderer.SetOptionFloat(CRenderer::OPT_LODBIAS, g_LodBias);

	// create terrain related stuff
	new CTextureManager;

	// create the material manager
	new CMaterialManager;

	MICROLOG(L"init renderer");
	g_Renderer.Open(g_xres,g_yres,g_bpp);

	// Setup lighting environment. Since the Renderer accesses the
	// lighting environment through a pointer, this has to be done before
	// the first Frame.
	g_Renderer.SetLightEnv(&g_LightEnv);

	// I haven't seen the camera affecting GUI rendering and such, but the
	// viewport has to be updated according to the video mode
	SViewPort vp;
	vp.m_X=0;
	vp.m_Y=0;
	vp.m_Width=g_xres;
	vp.m_Height=g_yres;
	g_Renderer.SetViewport(vp);

	ColorActivateFastImpl();
}

static void InitSDL()
{
	MICROLOG(L"init sdl");
	if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_NOPARACHUTE) < 0)
	{
		LOG(ERROR, LOG_CATEGORY, "SDL library initialization failed: %s", SDL_GetError());
		throw PSERROR_System_SDLInitFailed();
	}
	atexit(SDL_Quit);
	SDL_EnableUNICODE(1);
}


void EndGame()
{
	if (g_NetServer)
	{
		delete g_NetServer;
		g_NetServer=NULL;
	}
	else if (g_NetClient)
	{
		delete g_NetClient;
		g_NetClient=NULL;
	}

	delete g_Game;
	g_Game=NULL;
}


void Shutdown(uint flags)
{
	MICROLOG(L"Shutdown");

	if (g_Game)
		EndGame();

	ShutdownPs(); // Must delete g_GUI before g_ScriptingHost

	TIMER_BEGIN("shutdown Scheduler");
	delete &g_Scheduler;
	TIMER_END("shutdown Scheduler");

	delete &g_JSGameEvents;

	if (! (flags & INIT_NO_SIM))
	{
		TIMER_BEGIN("shutdown SessionManager");
		delete &g_SessionManager;
		TIMER_END("shutdown SessionManager");

		TIMER_BEGIN("shutdown mouse stuff");
		delete &g_Mouseover;
		delete &g_Selection;
		delete &g_BuildingPlacer;
		TIMER_END("shutdown mouse stuff");

		TIMER_BEGIN("shutdown Pathfinder");
		delete &g_Pathfinder;
		TIMER_END("shutdown Pathfinder");

		// Managed by CWorld
		// delete &g_EntityManager;

		TIMER_BEGIN("shutdown game scripting stuff");
		delete &g_GameAttributes;
		
		delete &g_TriggerManager;
		delete &g_FormationManager;
		delete &g_TechnologyCollection;
		delete &g_EntityFormationCollection;
		delete &g_EntityTemplateCollection;
		TIMER_END("shutdown game scripting stuff");
	}

	// destroy actor related stuff
	TIMER_BEGIN("shutdown actor stuff");
	delete &g_MaterialManager;
	TIMER_END("shutdown actor stuff");

	// destroy terrain related stuff
	TIMER_BEGIN("shutdown TexMan");
	delete &g_TexMan;
	TIMER_END("shutdown TexMan");

	// destroy renderer
	TIMER_BEGIN("shutdown Renderer");
	delete &g_Renderer;
	g_VBMan.Shutdown();
	TIMER_END("shutdown Renderer");

	TIMER_BEGIN("shutdown ScriptingHost");
	delete &g_ScriptingHost;
	TIMER_END("shutdown ScriptingHost");

	TIMER_BEGIN("shutdown ConfigDB");
	delete &g_ConfigDB;
	TIMER_END("shutdown ConfigDB");

	// Shut down the network loop
	TIMER_BEGIN("shutdown CSocketBase");
	CSocketBase::Shutdown();
	TIMER_END("shutdown CSocketBase");

	// Really shut down the i18n system. Any future calls
	// to translate() will crash.
	TIMER_BEGIN("shutdown I18N");
	I18n::Shutdown();
	TIMER_END("shutdown I18N");

	// resource
	// first shut down all resource owners, and then the handle manager.
	TIMER_BEGIN("resource modules");
		snd_shutdown();

		(void)trace_write_to_file("../logs/trace.txt");

		vfs_shutdown();

		// must come before h_mgr_shutdown - it frees IO buffers,
		// which we don't want showing up as leaks.
		file_shutdown();

		// this forcibly frees all open handles (thus preventing real leaks),
		// and makes further access to h_mgr impossible.
		h_mgr_shutdown();

		// must come after h_mgr_shutdown - it causes memory
		// to be freed, which requires this module to still be active.
		mem_shutdown();
	TIMER_END("resource modules");

	TIMER_BEGIN("shutdown misc");
		timer_display_client_totals();

		// should be last, since the above use them
		debug_shutdown();
		SAFE_DELETE(g_Logger);
		delete &g_Profiler;
		delete &g_ProfileViewer;
	TIMER_END("shutdown misc");
}


void Init(const CmdLineArgs& args, uint flags)
{
	const bool setup_vmode = (flags & INIT_HAVE_VMODE) == 0;

	MICROLOG(L"Init");

	debug_set_thread_name("main");
	// add all debug_printf "tags" that we are interested in:
	debug_filter_add("TIMER");

	// Query CPU capabilities, possibly set some CPU-dependent flags
	cpu_Init();

	// Do this as soon as possible, because it chdirs
	// and will mess up the error reporting if anything
	// crashes before the working directory is set.
	MICROLOG(L"init vfs");
	InitVfs(args);

	// This must come after VFS init, which sets the current directory
	// (required for finding our output log files).
	g_Logger = new CLogger;

	// Call LoadLanguage(NULL) to initialize the I18n system, but
	// without loading an actual language file - translate() will
	// just show the English key text, which is better than crashing
	// from a null pointer when attempting to translate e.g. error messages.
	// Real languages can only be loaded when the scripting system has
	// been initialised.
	//
	// this uses LOG and must therefore come after CLogger init.
	MICROLOG(L"init i18n");
	I18n::LoadLanguage(NULL);

	// override ah_translate with our i18n code.
	AppHooks hooks = {0};
	hooks.translate = psTranslate;
	hooks.translate_free = psTranslateFree;
	hooks.bundle_logs = psBundleLogs;
	hooks.get_log_dir = psGetLogDir;
	app_hooks_update(&hooks);

	// Set up the console early, so that debugging
	// messages can be logged to it. (The console's size
	// and fonts are set later in InitPs())
	g_Console = new CConsole();

	if(setup_vmode)
		InitSDL();

	// preferred video mode = current desktop settings
	// (command line params may override these)
	gfx_get_video_mode(&g_xres, &g_yres, &g_bpp, &g_freq);

	new CProfileViewer;
	new CProfileManager;	// before any script code

	MICROLOG(L"init scripting");
	InitScripting();	// before GUI

	// g_ConfigDB, command line args, globals
	CONFIG_Init(args);

	// setup_gui must be set after CONFIG_Init, so command-line parameters can disable it
	const bool setup_gui = ((flags & INIT_NO_GUI) == 0 && g_AutostartMap.empty());

	// GUI is notified in SetVideoMode, so this must come before that.
#ifndef NO_GUI
	new CGUI;
#endif

	bool windowed = false;
	CFG_GET_SYS_VAL("windowed", Bool, windowed);

	if(setup_vmode)
	{
		MICROLOG(L"SetVideoMode");
		if(SetVideoMode(g_xres, g_yres, 32, !windowed) < 0)
		{
			LOG(ERROR, LOG_CATEGORY, "Could not set %dx%d graphics mode: %s", g_xres, g_yres, SDL_GetError());
			throw PSERROR_System_VmodeFailed();
		}

		SDL_WM_SetCaption("0 A.D.", "0 A.D.");
	}

	tex_codec_register_all();

	uint quality = SANE_TEX_QUALITY_DEFAULT;	// TODO: set value from config file
	SetTextureQuality(quality);

	// required by ogl_tex to detect broken gfx card/driver combos
	gfx_detect();

	ogl_WarnIfError();

	if(!g_Quickstart)
	{
		WriteSystemInfo();
		// note: no longer vfs_display here. it's dog-slow due to unbuffered
		// file output and very rarely needed.
	}
	else
	{
		// speed up startup by disabling all sound
		// (OpenAL init will be skipped).
		// must be called before first snd_open.
		snd_disable(true);
	}

	// (must come after SetVideoMode, since it calls ogl_Init)
	const char* missing = ogl_HaveExtensions(0,
		"GL_ARB_multitexture",
		"GL_EXT_draw_range_elements",
		"GL_ARB_texture_env_combine",
		"GL_ARB_texture_env_dot3",
		0);
	if(missing)
	{
		wchar_t buf[500];
		const wchar_t* fmt =
			L"The %hs extension doesn't appear to be available on your computer."
			L" The game may still work, though - you are welcome to try at your own risk."
			L" If not or it doesn't look right, upgrade your graphics card.";
		swprintf(buf, ARRAY_SIZE(buf), fmt, missing);
		DISPLAY_ERROR(buf);
		// TODO: i18n
	}

	if (!ogl_HaveExtension("GL_ARB_texture_env_crossbar"))
	{
		DISPLAY_ERROR(
			L"The GL_ARB_texture_env_crossbar extension doesn't appear to be available on your computer."
			L" Shadows are not available and overall graphics quality might suffer."
			L" You are advised to try installing newer drivers and/or upgrade your graphics card.");
		g_Shadows = false;
	}

	// enable/disable VSync
	// note: "GL_EXT_SWAP_CONTROL" is "historical" according to dox.
#if OS_WIN
	if(ogl_HaveExtension("WGL_EXT_swap_control"))
		pwglSwapIntervalEXT(g_VSync? 1 : 0);
#endif

	MICROLOG(L"init ps");
	InitPs(setup_gui);

	ogl_WarnIfError();
	InitRenderer();

	if (! (flags & INIT_NO_SIM))
	{
		{
			TIMER("Init_entitiessection");
			// This needs to be done after the renderer has loaded all its actors...
			new CEntityTemplateCollection;
			new CFormationCollection;
			new CTechnologyCollection;
			g_EntityFormationCollection.LoadTemplates();
			g_TechnologyCollection.LoadTechnologies();
			new CFormationManager;
			new CTriggerManager;
			g_TriggerManager.LoadXml(CStr("scripts/TriggerSpecs.xml"));
			g_ScriptingHost.RunScript("scripts/trigger_functions.js");

			// CEntityManager is managed by CWorld
			//new CEntityManager;
			new CSelectedEntities;
			new CMouseoverEntities;
		}

		{
			TIMER("Init_miscgamesection");
			new CPathfindEngine;
			new CBuildingPlacer;
			new CSessionManager;
			new CGameAttributes;
		}

		// Register a few Game/Network JS globals
		g_ScriptingHost.SetGlobal("g_GameAttributes", OBJECT_TO_JSVAL(g_GameAttributes.GetScript()));
	}
	
	
	// Check for heap corruption after every allocation. Very, very slowly.
	// (And it highlights the allocation just after the one you care about,
	// so you need to run it again and tell it to break on the one before.)
//	debug_heap_enable(DEBUG_HEAP_ALL);

	InitInput();

	ogl_WarnIfError();

#ifndef NO_GUI
	{
	TIMER("Init_guiload");
	g_GUI.SendEventToAll("load");
	}
#endif

	if (g_FixedFrameTiming) {
		CCamera &camera = *g_Game->GetView()->GetCamera();
#if 0		// TOPDOWN
		camera.SetProjection(1.0f,10000.0f,DEGTORAD(90));
		camera.m_Orientation.SetIdentity();
		camera.m_Orientation.RotateX(DEGTORAD(90));
		camera.m_Orientation.Translate(CELL_SIZE*250*0.5, 250, CELL_SIZE*250*0.5);
#else		// std view
		camera.SetProjection(1.0f,10000.0f,DEGTORAD(20));
		camera.m_Orientation.SetXRotation(DEGTORAD(30));
		camera.m_Orientation.RotateY(DEGTORAD(-45));
		camera.m_Orientation.Translate(350, 350, -275);
#endif
		camera.UpdateFrustum();
	}

	if (! g_AutostartMap.empty())
	{
		// Code copied mostly from atlas/GameInterface/Handlers/Map.cpp -
		// maybe should be refactored to avoid duplication
		g_GameAttributes.m_MapFile = g_AutostartMap+".pmp";
		for (int i=1; i<8; ++i)
			g_GameAttributes.GetSlot(i)->AssignLocal();
		g_Game = new CGame();

		PSRETURN ret = g_Game->StartGame(&g_GameAttributes);
		debug_assert(ret == PSRETURN_OK);
		LDR_NonprogressiveLoad();
		ret = g_Game->ReallyStartGame();
		debug_assert(ret == PSRETURN_OK);
	}

}
