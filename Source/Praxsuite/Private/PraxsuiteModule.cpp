#include "Modules/ModuleManager.h"

// A default module implementation. There is nothing to do at load: the subsystem is created by the
// engine when the game instance starts, and configuration is read from Project Settings at that
// point rather than here. A module that did work at load time would run before the settings object
// is reliably available.
IMPLEMENT_MODULE(FDefaultModuleImpl, Praxsuite)
