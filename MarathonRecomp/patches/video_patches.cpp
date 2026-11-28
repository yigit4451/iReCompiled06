#include <api/Marathon.h>
#include <cpu/guest_thread.h>
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
// XMV / Criware movie player stall guard (SONIC TEAM logo -> opening movie -> Title Screen).
//
// sub_82743680 is the XMV "get next frame" call made by Sonicteam::MovieObjectWmv::Update (sub_8264C8D0). Its worker
// threads (0x827422F8..0x82742388) and the XAudio callback can lock each other up, leaving the game thread in an
// infinite KeWaitForSingleObject or leaving a worker thread suspended/holding an XAudio voice lock when the player is
// destroyed on transition to TitleTask.
// ---------------------------------------------------------------------------------------------------------------------
extern std::atomic<bool> g_xmvAbortRequested;
extern thread_local uint64_t g_xmvFrameCallStartMs;
uint64_t XmvNowMs();
bool KeSetEvent(XKEVENT* pEvent, uint32_t Increment, bool Wait);
uint32_t NtResumeThread(GuestThreadHandle* hThread, be<uint32_t>* suspendCount);

static constexpr uint32_t XMV_EOF_RESULT = 0x16660026; // XMEDIA_W_EOF = (5734 << 16) | 38
static constexpr uint32_t XMV_STATE_FINISHED = 7;
static std::atomic<uint32_t> g_xmvAbortedPlayer{ 0 };
static std::atomic<uint32_t> g_activeXmvPlayer{ 0 };
static std::atomic<uint64_t> g_xmvAbortDrainUntilMs{ 0 };

// Track CCalXmvPlayer* passed into CCalXmvPlayer::GetStatus (sub_82733640) so we can mute/quiesce its XAudioSourceVoice
// before MovieObjectWmv::Update destroys the player on EOF.
static std::atomic<uint32_t> g_lastCalXmvPlayer{ 0 };

static void XmvQuiesceCalAudioVoice(uint8_t* base, uint32_t calPlayerAddr)
{
    if (base == nullptr || calPlayerAddr < 0x10000u)
        return;

    auto* pCalPlayer = reinterpret_cast<CCalXmvPlayer*>(base + calPlayerAddr);
    if (!pCalPlayer->m_pChannel)
        return;

    auto* pAudioRenderer = pCalPlayer->m_pChannel->m_pAudioRenderer.get();
    if (!pAudioRenderer)
        return;

    auto* pSourceVoice = pAudioRenderer->m_pSourceVoice.get();
    if (!pSourceVoice)
        return;

    // Mute and silence XAudioSourceVoice immediately so AudioThread (sub_8255B9C0 -> XAudio worker 0x8255B848)
    // stops pulling XMV packets or contending on the XMV stream CriticalSection while MovieObjectWmv tears down.
    pSourceVoice->m_IsMuted = true;
    pSourceVoice->m_Volume = 0.0f;
}

static void XmvFinishPlayer(uint8_t* base, uint32_t player)
{
    g_xmvAbortDrainUntilMs.store(XmvNowMs() + 3000u, std::memory_order_release);

    if (base == nullptr || player < 0x10000u)
        return;

    // Quiesce the wrapping CCalXmvPlayer's XAudio voice first so AudioThread never blocks during teardown.
    const uint32_t calPlayer = g_lastCalXmvPlayer.load(std::memory_order_relaxed);
    if (calPlayer != 0)
        XmvQuiesceCalAudioVoice(base, calPlayer);

    // Raise stop bit (0x4) checked by the stream reader / audio feeder / video decoder / audio renderer worker loops
    // (player + 252, 256, 260 in IXMediaXmvPlayer, plus 220, 224, 228 if offset from subobject).
    for (const uint32_t offset : { 220u, 224u, 228u, 252u, 256u, 260u })
    {
        auto* flags = reinterpret_cast<be<uint32_t>*>(base + player + offset);
        const uint32_t cur = flags->get();
        if (cur <= 0xFFu)
            *flags = cur | 0x4u;
    }

    // Resume any worker threads (player + 264..276) that were created with CREATE_SUSPENDED (e.g. 0x82742388)
    // so MovieObjectWmv's destructor never deadlocks joining an unresumed thread!
    for (const uint32_t handleOffset : { 264u, 268u, 272u, 276u })
    {
        const uint32_t hVal = reinterpret_cast<be<uint32_t>*>(base + player + handleOffset)->get();
        if (hVal != 0 && hVal != GUEST_INVALID_HANDLE_VALUE && IsKernelObject(hVal))
        {
            auto* hThread = GetKernelObject<GuestThreadHandle>(hVal);
            be<uint32_t> prevSuspend = 0;
            NtResumeThread(hThread, &prevSuspend);
        }
    }

    // Wake every internal XKEVENT (+84..+196, 16-byte stride) ONLY if already initialized by the guest
    // (WaitListHead.Flink == OBJECT_SIGNATURE or valid Type 0/1 with SignalState <= 1) so we never corrupt adjacent fields.
    for (uint32_t offset = 84; offset <= 196; offset += 16)
    {
        auto* event = reinterpret_cast<XKEVENT*>(base + player + offset);
        if (event->WaitListHead.Flink == OBJECT_SIGNATURE ||
            ((event->Type == 0 || event->Type == 1) && event->SignalState.get() <= 1u))
        {
            KeSetEvent(event, 0, false);
        }
    }
}

void XmvForceAbortActivePlayer()
{
    g_xmvAbortDrainUntilMs.store(XmvNowMs() + 3000u, std::memory_order_release);

    if (g_memory.base == nullptr)
        return;

    uint32_t player = g_activeXmvPlayer.load(std::memory_order_acquire);
    if (player >= 0x10000u)
    {
        g_xmvAbortedPlayer.store(player, std::memory_order_release);
        XmvFinishPlayer(g_memory.base, player);
    }
    else
    {
        const uint32_t calPlayer = g_lastCalXmvPlayer.load(std::memory_order_relaxed);
        if (calPlayer >= 0x10000u)
            XmvQuiesceCalAudioVoice(g_memory.base, calPlayer);
    }
}

PPC_FUNC_IMPL(__imp__sub_82743680);
PPC_FUNC(sub_82743680)
{
    const uint32_t player = ctx.r3.u32;
    const uint32_t prevActive = g_activeXmvPlayer.load(std::memory_order_acquire);
    if (player >= 0x10000u)
    {
        if (prevActive != 0 && prevActive != player)
            g_xmvAbortDrainUntilMs.store(0, std::memory_order_release);
        g_activeXmvPlayer.store(player, std::memory_order_release);
    }

    const uint32_t prevAborted = g_xmvAbortedPlayer.load(std::memory_order_acquire);

    // A different player means a new movie started; forget the previous abort cleanly.
    if (prevAborted != 0 && prevAborted != player)
    {
        g_xmvAbortedPlayer.store(0, std::memory_order_release);
        g_xmvAbortRequested.store(false, std::memory_order_release);
        g_xmvAbortDrainUntilMs.store(0, std::memory_order_release);
    }

    if (g_xmvAbortedPlayer.load(std::memory_order_acquire) == player && player != 0)
    {
        XmvFinishPlayer(base, player);
        ctx.r3.u32 = XMV_EOF_RESULT;
        return;
    }

    g_xmvFrameCallStartMs = XmvNowMs();
    __imp__sub_82743680(ctx, base);
    g_xmvFrameCallStartMs = 0;

    if (g_xmvAbortRequested.load(std::memory_order_acquire))
    {
        LOGN_WARNING("XMV: finishing stalled movie playback cleanly (EOF + audio/worker drain).");

        g_xmvAbortedPlayer.store(player, std::memory_order_release);
        XmvFinishPlayer(base, player);
        // Clear the global abort flag once the stalled player has been transitioned to EOF so subsequent
        // movies or audio streams on the Title Screen are completely unaffected.
        g_xmvAbortRequested.store(false, std::memory_order_release);
        ctx.r3.u32 = XMV_EOF_RESULT;
    }
}

// CCalXmvPlayer::GetStatus (sub_82733640) queried by MovieObjectWmv::Update (status 7 => finished, player is destroyed).
PPC_FUNC_IMPL(__imp__sub_82733640);
PPC_FUNC(sub_82733640)
{
    const uint32_t calPlayer = ctx.r3.u32;
    const uint32_t pStatus = ctx.r4.u32;

    if (calPlayer >= 0x10000u)
        g_lastCalXmvPlayer.store(calPlayer, std::memory_order_relaxed);

    if (g_xmvAbortedPlayer.load(std::memory_order_acquire) != 0 && pStatus >= 0x10000u)
    {
        g_xmvAbortDrainUntilMs.store(XmvNowMs() + 3000u, std::memory_order_release);
        XmvQuiesceCalAudioVoice(base, calPlayer);
        *reinterpret_cast<be<uint32_t>*>(base + pStatus) = XMV_STATE_FINISHED;
        // Clear g_xmvAbortedPlayer after reporting FINISHED so MovieObjectWmv::Update tears down this movie
        // once and future movies start fresh.
        g_xmvAbortedPlayer.store(0, std::memory_order_release);
        g_activeXmvPlayer.store(0, std::memory_order_release);
        ctx.r3.u32 = 0;
        return;
    }

    __imp__sub_82733640(ctx, base);
}

// Guard XMV worker helper sub_827441B8 (called on XMV decoder thread 0x82742358 via
// sub_827442E8 -> sub_82742B90 -> sub_827428B0 -> sub_827441B8) against NULL r3 (0x00000004 / 0x7000000004 SIGBUS)
// and post-abort teardown races while worker threads finish their final iteration and exit.
PPC_FUNC_IMPL(__imp__sub_827441B8);
PPC_FUNC(sub_827441B8)
{
    const uint32_t ptr = ctx.r3.u32;
    if (base == nullptr || ptr < 0x10000u || ptr >= 0xF0000000u)
    {
        ctx.r3.u32 = 0;
        return;
    }

    if (g_xmvAbortRequested.load(std::memory_order_acquire) ||
        g_xmvAbortedPlayer.load(std::memory_order_acquire) != 0 ||
        XmvNowMs() < g_xmvAbortDrainUntilMs.load(std::memory_order_acquire))
    {
        ctx.r3.u32 = 0;
        return;
    }

    __imp__sub_827441B8(ctx, base);
}
