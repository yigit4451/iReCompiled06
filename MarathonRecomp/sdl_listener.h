#pragma once

class ISDLEventListener
{
public:
    virtual ~ISDLEventListener() = default;
    virtual bool OnSDLEvent(SDL_Event* event) = 0;
};

extern std::vector<ISDLEventListener*>& GetEventListeners();
void RegisterSDLEventListener(ISDLEventListener* listener);
void UnregisterSDLEventListener(ISDLEventListener* listener);
bool DispatchSDLEvent(SDL_Event* event);

class SDLEventListener : public ISDLEventListener
{
public:
    SDLEventListener()
    {
        RegisterSDLEventListener(this);
    }

    ~SDLEventListener() override
    {
        UnregisterSDLEventListener(this);
    }

    bool OnSDLEvent(SDL_Event* event) override { return false; }
};
