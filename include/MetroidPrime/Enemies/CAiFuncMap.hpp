#ifndef _CAIFUNCMAP
#define _CAIFUNCMAP

#include "types.h"
#include <rstl/pair.hpp>
#include <rstl/vector.hpp>

enum EStateMsg {
  kStateMsg_Activate = 0,
  kStateMsg_Update = 1,
  kStateMsg_Deactivate = 2,
};

#ifdef TARGET_PC
// Port: the pointer-to-member typedefs below are declared while CAi is still
// incomplete. Under the MSVC ABI (clang-cl) a translation unit that first needs
// their layout before seeing CAi's definition (CStateMachine) uses the most
// general 24-byte representation, while one that sees the definition first
// (CAi.cpp, which fills and reads the function map) uses the 8-byte
// single-inheritance one. CAi::GetStateFunc then returns its 8-byte value in a
// register while the caller reads a 24-byte temporary it never wrote: every
// AI state function came back as stack garbage, and a Seedling's "Start" state
// crashed in CStateMachineState::SetState. CAi has a single inheritance chain
// (CPhysicsActor, CActor, CEntity), so pin that model for every unit.
class __single_inheritance CAi;
#else
class CAi;
#endif
class CStateManager;
typedef void (CAi::*CAiStateFunc)(CStateManager& mgr, EStateMsg msg, float arg);
typedef bool (CAi::*CAiTriggerFunc)(CStateManager& mgr, float arg);

class CAiFuncMap {
public:
  CAiFuncMap();
  ~CAiFuncMap() {}

  const CAiStateFunc GetStateFunc(const char* state) const;
  const CAiTriggerFunc GetTriggerFunc(const char* state) const;
#ifdef TARGET_PC
  // Port: the tables have been found overwritten at run time (a Seedling's
  // "Start" state received a garbage function pointer and crashed in
  // CStateMachineState::SetState). Checks every entry against the static
  // tables, logs the first corruption and restores the entries, so a lookup
  // never hands out garbage. Called from every lookup and once per tick.
  void Verify(const char* when) const;
#endif

private:
  rstl::vector< rstl::pair< const char*, CAiStateFunc > > x0_states;
  rstl::vector< rstl::pair< const char*, CAiTriggerFunc > > x10_triggers;
};

#endif // _CAIFUNCMAP
