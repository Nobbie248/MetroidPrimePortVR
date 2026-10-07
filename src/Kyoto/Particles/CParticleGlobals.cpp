#include "Kyoto/Particles/CParticleGlobals.hpp"

int CParticleGlobals::mParticleLifetime = 0.f;
float CParticleGlobals::mParticleLifetimeReal = 0.f;
int CParticleGlobals::mEmitterTime = 0;
float CParticleGlobals::mEmitterTimeReal = 0.f;
int CParticleGlobals::mParticleLifetimePercentage = 0;
float CParticleGlobals::mParticleLifetimePercentageReal = 0.f;
float CParticleGlobals::mParticleLifetimePercentageRemainder = 0.f;
CElementGen::CParticle* CParticleGlobals::mCurrentParticle = nullptr;
float* CParticleGlobals::mParticleAccessParameters = nullptr;
CParticleGlobals::SParticleSystem* CParticleGlobals::mCurrentParticleSystem = nullptr;
#ifdef TARGET_PC
CElementGen::CParticle* CParticleGlobals::xPortUVParticle = nullptr;
const CElementGen::CParticle* CParticleGlobals::xPortIrndParticle = nullptr;

// splitmix64 of (seed, element address): the same pair always gives the same bits.
u32 CParticleGlobals::PortIrndBits(const CElementGen::CParticle* particle, const void* element) {
  uint64_t z = (static_cast< uint64_t >(particle->xPortSeed) << 32) ^
               static_cast< uint64_t >(reinterpret_cast< uintptr_t >(element));
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast< u32 >(z >> 32);
}

float CParticleGlobals::PortIrndUnit(const CElementGen::CParticle* particle, const void* element) {
  return static_cast< float >(PortIrndBits(particle, element) >> 8) * (1.f / 16777216.f);
}
#endif

void CParticleGlobals::SetParticleLifetime(int lifetime) {
  mParticleLifetime = lifetime;
  mParticleLifetimeReal = static_cast< float >(lifetime);
}

void CParticleGlobals::SetEmitterTime(int time) {
  mEmitterTime = time;
  mEmitterTimeReal = static_cast< float >(time);
}

void CParticleGlobals::UpdateParticleLifetimeTweenValues(int time) {
  float d = mParticleLifetime != 0.f ? mParticleLifetime : 1.f;
  mParticleLifetimePercentageReal = time * 100.f / d;
  mParticleLifetimePercentage = mParticleLifetimePercentageReal;
  mParticleLifetimePercentageRemainder =
      mParticleLifetimePercentageReal - mParticleLifetimePercentage;
  if (mParticleLifetimePercentage < 0) {
    mParticleLifetimePercentage = 0;
  } else if (mParticleLifetimePercentage > 100) {
    mParticleLifetimePercentage = 100;
  }
}
