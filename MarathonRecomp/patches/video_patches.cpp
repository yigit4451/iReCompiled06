#include <api/Marathon.h>
#include <gpu/render_resolution.h>
#include <gpu/video.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>
#include <os/logger.h>
#include <user/config.h>
#include <app.h>
#include <atomic>

const char* g_pBlockName{};

extern const char* g_surfaceCreationName;

void BeginBlockGetName(PPCRegister& r3)
{
    g_pBlockName = reinterpret_cast<const char*>(g_memory.Translate(r3.u32));

#if _DEBUG
    if (g_pBlockName)
        LOGFN_UTILITY("Block Begin: {}", g_pBlockName);
#endif
}

// Sonicteam::RenderAction::ReadBlock
PPC_FUNC_IMPL(__imp__sub_8260DF88);
PPC_FUNC(sub_8260DF88)
{
    if (!Video::s_needsResize)
    {
        const auto pFilePath = reinterpret_cast<stdx::string*>(base + ctx.r4.u32);

        // Don't process render script if it's the same one currently in use.
        if (pFilePath && Sonicteam::Globals::ms_pCurrentRenderScript && *pFilePath == *Sonicteam::Globals::ms_pCurrentRenderScript)
            return;
    }

    __imp__sub_8260DF88(ctx, base);
}

// EndBlock
PPC_FUNC_IMPL(__imp__sub_826078D8);
PPC_FUNC(sub_826078D8)
{
#if _DEBUG
    if (g_pBlockName)
        LOGFN_UTILITY("Block End: {}", g_pBlockName);
#endif

    g_pBlockName = nullptr;

    __imp__sub_826078D8(ctx, base);
}

static void ApplyBufferSize(PPCContext& ctx, const char* pName)
{
    if (auto size = RenderResolution::FindBufferSize(pName, g_pBlockName))
    {
        ctx.r5.u32 = size->Width;
        ctx.r6.u32 = size->Height;
    }
}

// CreateTexture
PPC_FUNC_IMPL(__imp__sub_82619D00);
PPC_FUNC(sub_82619D00)
{
    const auto pName = reinterpret_cast<stdx::string*>(g_memory.Translate(ctx.r4.u32));

    ApplyBufferSize(ctx, pName->c_str());

#if _DEBUG
    const auto width = ctx.r5.u32;
    const auto height = ctx.r6.u32;
#endif

    g_surfaceCreationName = pName->c_str();
    __imp__sub_82619D00(ctx, base);
    g_surfaceCreationName = nullptr;

#if _DEBUG
    LOGFN_UTILITY("Created texture: {} ({}x{})", pName->c_str(), width, height);
#endif
}

// CreateDepthStencilSurface
PPC_FUNC_IMPL(__imp__sub_82619B88);
PPC_FUNC(sub_82619B88)
{
    const auto pName = reinterpret_cast<stdx::string*>(g_memory.Translate(ctx.r4.u32));

    ApplyBufferSize(ctx, pName->c_str());

#if _DEBUG
    const auto width = ctx.r5.u32;
    const auto height = ctx.r6.u32;
#endif

    g_surfaceCreationName = pName->c_str();
    __imp__sub_82619B88(ctx, base);
    g_surfaceCreationName = nullptr;

#if _DEBUG
    if (g_pBlockName)
    {
        LOGFN_UTILITY("Created texture for {}: {} ({}x{})", g_pBlockName, pName->c_str(), width, height);
    }
    else
    {
        LOGFN_UTILITY("Created texture: {} ({}x{})", pName->c_str(), width, height);
    }
#endif
}

// CreateArrayTexture
PPC_FUNC_IMPL(__imp__sub_82619FF0);
PPC_FUNC(sub_82619FF0)
{
    const auto pName = reinterpret_cast<stdx::string*>(g_memory.Translate(ctx.r4.u32));

    if (*pName == "csm")
    {
        if (!Video::s_needsShadowMapResize && Video::RestoreCachedRenderTarget("csm", RenderTargetType::PostEffect))
        {
            ctx.r3.u32 = 1;
            return;
        }
    }

    ApplyBufferSize(ctx, pName->c_str());

#if _DEBUG
    const auto width = ctx.r5.u32;
    const auto height = ctx.r6.u32;
    const auto count = ctx.r7.u32;
#endif

    g_surfaceCreationName = pName->c_str();
    __imp__sub_82619FF0(ctx, base);
    g_surfaceCreationName = nullptr;

#if _DEBUG
    LOGFN_UTILITY("Created array texture: {} ({}x{}x{})", pName->c_str(), width, height, count);
#endif
}

// Sonicteam::MyReflectionArea::Update
PPC_FUNC_IMPL(__imp__sub_822740A8);
PPC_FUNC(sub_822740A8)
{
    const auto pMyReflectionArea = reinterpret_cast<Sonicteam::MyReflectionArea*>(base + ctx.r3.u32);

    // Force planar reflections to draw all objects and terrain.
    pMyReflectionArea->m_ClumpCutoffCoverage = 0.0f;
    pMyReflectionArea->m_SectorCutoffCoverage = 0.0f;

    __imp__sub_822740A8(ctx, base);

    if (pMyReflectionArea->m_IsInsideArea)
        return;

    const auto pGame = static_cast<Sonicteam::GameImp*>(pMyReflectionArea->m_spGame.get());
    const auto pConvexShape = static_cast<Sonicteam::SoX::Physics::Havok::ConvexShapeHavok*>(pMyReflectionArea->m_pConvexShape.get());

    for (const auto& rActorId : pGame->m_PlayerActorIDs)
    {
        const auto pPlayer = pGame->m_spActorManager->Find<Sonicteam::Player::Object>(rActorId);

        if (!pPlayer)
            continue;

        const auto pCameraman = static_cast<Sonicteam::Camera::Cameraman*>(pPlayer->m_pCameraman.get());

        if (!pCameraman)
            continue;

        // Fix reflection areas not updating with camera inside.
        pMyReflectionArea->m_IsInsideArea = pConvexShape->m_pConvexVerticesShape->hasPoint(pCameraman->m_Transform.m_Position);
    }
}

std::string g_renderWorldFBO;

void GetRenderWorldFBO(PPCRegister& name)
{
    const auto pName = xpointer(reinterpret_cast<char*>(name.u32));
    g_renderWorldFBO = std::string(pName.get());
}

void FurtherObjectShadows(PPCRegister& scope)
{
    if (g_renderWorldFBO != "shadowmap")
        return;

    scope.u32 = 1;
}

bool DisableRadialBlur()
{
    return Config::RadialBlur == ERadialBlur::Off;
}

bool DisableKingdomValleyMist()
{
    return Config::DisableKingdomValleyMist;
}

static thread_local bool g_isLoadingEnvMap = false;

void SceneEnvMapLoadTexture()
{
    if (Config::FixEnvironmentMaps)
        g_isLoadingEnvMap = true;
}

void ResourceMgrKeyOverride(PPCRegister& r1, PPCRegister& r28)
{
    if (!g_isLoadingEnvMap)
        return;

    g_isLoadingEnvMap = false;

    // std::string::assign(const std::string&, uint, uint)
    GuestToHostFunction<void>(0x82161D10, r1.u32 + 0x60, r28.u32, 0, 0xFFFFFFFF);
}

void IndexBufferLengthMidAsmHook(PPCRegister& r3)
{
    r3.u64 *= 2;
}

void SetShadowResolutionMidAsmHook(PPCRegister& r11)
{
    const auto res = (int32_t)Config::ShadowResolution.Value;

    if (res > 0)
        r11.u64 = res;
}

void WorldIntersectionCSM_SetCameraAspectRatio(PPCRegister& r4)
{
    const auto pApp = App::s_pApp;

    // Just in case.
    if (!pApp || !pApp->m_pDoc || !pApp->m_pDoc->m_pGameRuleContext)
        return;

    if (pApp->m_pDoc->m_pGameRuleContext->m_PlayerCount <= 1)
        return;

    const auto pGame = pApp->GetGame();

    if (!pGame)
        return;

    const auto pAspectRatio = reinterpret_cast<Sonicteam::SoX::Math::Vector2*>(g_memory.Translate(r4.u32));

    // Extend shadow map draw distance in multiplayer by 5x for parity with single player.
    // The game uses an identical shadow map config between single player and multiplayer,
    // even the cameras use the same parameters, so the distance being shorter is likely
    // down to something weird going on with there being more than one screen.
    pAspectRatio->X = pAspectRatio->X * 5.0f;
    pAspectRatio->Y = pAspectRatio->Y * 5.0f;
}

void SetRenderConfig(PPCRegister& r1)
{
    const auto pLuaSystem = reinterpret_cast<xpointer<Sonicteam::LuaSystem>*>(g_memory.Translate(r1.u32 + 0x54))->get();

    // Force MSAA type to 2x to override stages that disable it.
    pLuaSystem->SetString("MSAAType", "2x");

    // Force shadow cascades to 4.
    // "GenerateCSMObjects" gets passed in as the third argument to "RenderCSM",
    // but that function doesn't do anything with it, so we only need to change this.
    pLuaSystem->SetInteger("GenerateCSMLevels", 4);
}

void SetMsaaLevelForBuffers(PPCRegister& r31, PPCRegister& r4)
{
    const auto L = reinterpret_cast<lua50::lua_State*>(g_memory.Translate(r31.u32));

    // MSAA type is provided, abort.
    if (lua50::lua_isstring(L, 9))
        return;

    const auto pBufferName = lua50::lua_tostring(L, 2);
    const auto pDepthStencilName = lua50::lua_tostring(L, 3);
    const auto bufferType = Sonicteam::SoX::Graphics::GetBufferTypeByName(lua50::lua_tostring(L, 4));

    static const std::array<std::tuple<const char*, const char*, Sonicteam::SoX::Graphics::BufferType>, 2> buffers =
    {{
        // Force MSAA level for multiplayer.
        { "coframebuffer", "codepthstencil", Sonicteam::SoX::Graphics::BufferType_All },

        // Force MSAA level for radar.
        { "radermap", "depthstencil_256", Sonicteam::SoX::Graphics::BufferType_ColorDepth }
    }};

    for (const auto& [rBufferName, rDepthStencilName, rBufferType] : buffers)
    {
        if (strstr(pBufferName, rBufferName) && strcmp(pDepthStencilName, rDepthStencilName) == 0 && bufferType == rBufferType)
            r4.u32 = Config::AntiAliasing != EAntiAliasing::Off;
    }
}

void FixSurfaceSize(PPCRegister& r3, PPCRegister& r4)
{
    const auto internalSize = RenderResolution::GetInternalSize();

    r3.u32 = internalSize.Width;
    r4.u32 = internalSize.Height;
}

void FixFrameBufferTileRects(PPCRegister& r26, PPCRegister& r1)
{
    const auto pFrameBufferObjectXenon = reinterpret_cast<Sonicteam::SoX::Graphics::Xenon::FrameBufferObjectXenon*>(g_memory.Translate(r26.u32));
    const auto pSurfaceDesc = reinterpret_cast<D3DXBSURFACE_DESC*>(g_memory.Translate(r1.u32 + 0x80));

    // Tiling is disabled, always use a single rect.
    D3DXBRECT rect{ 0, 0, pSurfaceDesc->Width.get(), pSurfaceDesc->Height.get() };
    pFrameBufferObjectXenon->m_TileRects.push_back(rect);
}

bool ClearMsaaFrameBuffer(PPCRegister& r31)
{
    const auto pFrameBufferObjectXenon = reinterpret_cast<Sonicteam::SoX::Graphics::Xenon::FrameBufferObjectXenon*>(g_memory.Translate(r31.u32));

    // Ensure an MSAA framebuffer is cleared before re-use.
    return pFrameBufferObjectXenon->m_MSAALevel != 0;
}

void ResolveMsaaFrameBuffers(PPCRegister& r31, PPCRegister& r8)
{
    const auto pFrameBufferObjectXenon = reinterpret_cast<Sonicteam::SoX::Graphics::Xenon::FrameBufferObjectXenon*>(g_memory.Translate(r31.u32));

    // Set bit mask to always resolve color targets for MSAA framebuffers.
    if (pFrameBufferObjectXenon->m_MSAALevel)
        r8.u32 = 0xF;
}

bool CopyResolvedMsaaFrameBuffer(PPCRegister& r27)
{
    const auto pFrameBufferObjectXenon = *reinterpret_cast<xpointer<Sonicteam::SoX::Graphics::Xenon::FrameBufferObjectXenon>*>(g_memory.Translate(r27.u32));

    // Ensure an MSAA framebuffer's resolved output is copied.
    return pFrameBufferObjectXenon->m_MSAALevel != 0;
}

static GuestShader* g_movieVertexShader;
static GuestShader* g_moviePixelShaderHD;
static GuestShader* g_moviePixelShaderSD;

static int ScreenShaderInit(be<uint32_t>* a1)
{
    if (g_movieVertexShader == nullptr)
    {
        g_movieVertexShader = g_userHeap.AllocPhysical<GuestShader>();
        g_movieVertexShader->type = ResourceType::VertexShader;
    }

    if (g_moviePixelShaderHD == nullptr)
    {
        g_moviePixelShaderHD = g_userHeap.AllocPhysical<GuestShader>();
        g_moviePixelShaderHD->type = ResourceType::PixelShader;
    }

    if (g_moviePixelShaderSD == nullptr)
    {
        g_moviePixelShaderSD = g_userHeap.AllocPhysical<GuestShader>();
        g_moviePixelShaderSD->type = ResourceType::PixelShader;
    }

    g_moviePixelShaderHD->AddRef();
    g_moviePixelShaderSD->AddRef();
    g_movieVertexShader->AddRef();

    a1[0x12] = g_memory.MapVirtual(g_movieVertexShader);
    a1[0x51] = g_memory.MapVirtual(g_moviePixelShaderHD);
    a1[0x52] = g_memory.MapVirtual(g_moviePixelShaderSD);

    return 0;
}

GUEST_FUNCTION_HOOK(sub_82736178, ScreenShaderInit);

// ---------------------------------------------------------------------------------------------------------------------
// XMV / Criware movie player stall guard (SONIC TEAM logo -> opening movie).
//
// sub_82743680 is the XMV "get next frame" call made by Sonicteam::MovieObjectWmv::Update (sub_8264C8D0). Its worker
// threads (0x827422F8..0x82742388) and the XAudio callback can lock each other up, leaving the game thread in an
// infinite KeWaitForSingleObject. KeWaitForSingleObject (imports.cpp) slices waits made from the XMV code range and
// aborts them once this call has stalled for XMV_STALL_LIMIT_MS; this hook then finishes the movie cleanly by
// raising the player's stop bits, waking its internal events and reporting XMEDIA_W_EOF / state 7 (Finished), so the
// game advances to the title screen instead of hanging.
// ---------------------------------------------------------------------------------------------------------------------
extern std::atomic<bool> g_xmvAbortRequested;
extern thread_local uint64_t g_xmvFrameCallStartMs;
uint64_t XmvNowMs();
bool KeSetEvent(XKEVENT* pEvent, uint32_t Increment, bool Wait);

static constexpr uint32_t XMV_EOF_RESULT = 0x16660026; // XMEDIA_W_EOF = (5734 << 16) | 38
static constexpr uint32_t XMV_STATE_FINISHED = 7;
static uint32_t g_xmvAbortedPlayer = 0;

static void XmvFinishPlayer(uint8_t* base, uint32_t player)
{
    if (!player)
        return;

    // Stop bit (0x4) checked by the stream reader / audio feeder / video decoder worker loops.
    for (const uint32_t offset : { 220u, 224u, 228u })
    {
        auto* flags = reinterpret_cast<be<uint32_t>*>(base + player + offset);
        *flags = flags->get() | 0x4u;
    }

    // Wake every internal auto-reset event (+84..+196, 16-byte stride) so blocked workers observe the stop bits.
    for (uint32_t offset = 84; offset <= 196; offset += 16)
    {
        auto* event = reinterpret_cast<XKEVENT*>(base + player + offset);

        if (event->Header.Type == 0 || event->Header.Type == 1)
            KeSetEvent(event, 0, false);
    }
}

PPC_FUNC_IMPL(__imp__sub_82743680);
PPC_FUNC(sub_82743680)
{
    const uint32_t player = ctx.r3.u32;

    // A different player means a new movie started; forget the previous abort.
    if (g_xmvAbortedPlayer != 0 && g_xmvAbortedPlayer != player)
    {
        g_xmvAbortedPlayer = 0;
        g_xmvAbortRequested.store(false, std::memory_order_release);
    }

    if (g_xmvAbortedPlayer == player && player != 0)
    {
        ctx.r3.u32 = XMV_EOF_RESULT;
        return;
    }

    g_xmvFrameCallStartMs = XmvNowMs();
    __imp__sub_82743680(ctx, base);
    g_xmvFrameCallStartMs = 0;

    if (g_xmvAbortRequested.load(std::memory_order_acquire))
    {
        LOGN_WARNING("XMV: finishing stalled movie playback (EOF).");

        g_xmvAbortedPlayer = player;
        XmvFinishPlayer(base, player);
        ctx.r3.u32 = XMV_EOF_RESULT;
    }
}

// XMV playback status query used by MovieObjectWmv::Update (status 7 => finished, player is destroyed).
PPC_FUNC_IMPL(__imp__sub_82733640);
PPC_FUNC(sub_82733640)
{
    const uint32_t player = ctx.r3.u32;
    const uint32_t pStatus = ctx.r4.u32;

    if (g_xmvAbortedPlayer != 0 && g_xmvAbortedPlayer == player && pStatus != 0)
    {
        *reinterpret_cast<be<uint32_t>*>(base + pStatus) = XMV_STATE_FINISHED;
        ctx.r3.u32 = 0;
        return;
    }

    __imp__sub_82733640(ctx, base);
}
