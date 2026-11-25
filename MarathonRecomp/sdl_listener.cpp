#include "sdl_listener.h"

namespace
{
    std::mutex& GetEventListenersMutex()
    {
        static std::mutex s_mutex;
        return s_mutex;
    }
}

std::vector<ISDLEventListener*>& GetEventListeners()
{
    static std::vector<ISDLEventListener*> g_eventListeners;
    return g_eventListeners;
}

void RegisterSDLEventListener(ISDLEventListener* listener)
{
    if (!listener)
        return;

    std::lock_guard<std::mutex> lock(GetEventListenersMutex());
    auto& listeners = GetEventListeners();
    if (std::find(listeners.begin(), listeners.end(), listener) == listeners.end())
        listeners.emplace_back(listener);
}

void UnregisterSDLEventListener(ISDLEventListener* listener)
{
    if (!listener)
        return;

    std::lock_guard<std::mutex> lock(GetEventListenersMutex());
    auto& listeners = GetEventListeners();
    auto it = std::find(listeners.begin(), listeners.end(), listener);
    if (it != listeners.end())
        listeners.erase(it);
}

bool DispatchSDLEvent(SDL_Event* event)
{
    if (!event)
        return false;

    std::vector<ISDLEventListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(GetEventListenersMutex());
        snapshot = GetEventListeners();
    }

    for (auto* listener : snapshot)
    {
        if (listener && listener->OnSDLEvent(event))
            return true;
    }

    return false;
}
