/*
 * A break must stop a looping voice on the PC mixer.
 *
 * A charge beam's hum is a looping sample. The original DSP retires a broken
 * voice in a per-frame pass before it mixes anything (salBuildCommandList in
 * hw_dspctrl.c): any voice whose break arrived is deactivated outright. The PC
 * mixer had no such pass and relied on the ADSR release running to its end.
 *
 * That cannot finish: changed[] is only cleared by hwInitSamplePlayback, so the
 * break bit stays set on a running voice, every frame re-runs adsrStartRelease,
 * and adsr.cnt is reset before adsrHandle can count it down. The looping voice
 * then never reached salDeactivateVoice and kept sounding with no synth voice
 * left to stop it (reported on the tablet, any beam, often after being hit
 * mid-charge).
 *
 * These drive the real PC mixer over a synthetic looping sample: no game data
 * and no disc are involved.
 */

#include <musyx/hardware.h>
#include <musyx/sal.h>
#include <musyx/synthdata.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_VOICES 8
#define TEST_STUDIO 0
#define SMP_ID 0x215

/* Defined in hardware.c but absent from musyx/hardware.h; hwBreak picks the
 * changed[] slot from it, so the test has to drive it directly. */
void hwSetTimeOffset(u8 offset);

static void* testMalloc(size_t len) { return malloc(len); }
static void testFree(void* addr) { free(addr); }

/* The mixer's frame is 160 samples over 5 subframes (SAL_SUBFRAMES in hw_pc.c).
 * A break takes effect no later than the frame after the one it lands in. */
#define FRAME_SAMPLES 160
#define MAX_FRAMES 64

static int gFailures;
static int gMessageCount;
static u32 gLastMessage;

static void check(int ok, const char* what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

static u32 onMessage(u32 mesg, u32 userValue) {
  ++gMessageCount;
  gLastMessage = mesg;
  (void)userValue;
  return 0;
}

/* One frame of mixing into a scratch buffer. */
static void mixFrame(s16* dest) {
  memset(dest, 0, (size_t)FRAME_SAMPLES * 2 * sizeof(s16));
  salCtrlDsp(dest);
}

/* Runs frames until the voice leaves the running state, or gives up. Returns
 * the number of frames it took, or -1 if the voice was still running. */
static int runUntilStopped(int voice) {
  static s16 dest[FRAME_SAMPLES * 2];
  int frames;
  for (frames = 0; frames < MAX_FRAMES; ++frames) {
    mixFrame(dest);
    if (dspVoice[voice].state == 0) {
      return frames + 1;
    }
  }
  return -1;
}

/* Mixes a few frames so a freshly started voice reaches state 2 (running). */
static int runUntilRunning(int voice) {
  static s16 dest[FRAME_SAMPLES * 2];
  int frames;
  for (frames = 0; frames < MAX_FRAMES; ++frames) {
    mixFrame(dest);
    if (dspVoice[voice].state == 2) {
      return frames + 1;
    }
    if (dspVoice[voice].state == 0) {
      return -1; /* died on the way up */
    }
  }
  return -1;
}

/* Starts voice `voice` on a looped synthetic sample and leaves it running. */
static void startLoopingVoice(int voice, const short* pcm, unsigned length,
                              unsigned loop, unsigned loopLength) {
  SAMPLE_INFO info;
  memset(&info, 0, sizeof(info));
  info.addr = (void*)pcm;
  info.offset = 0;
  info.length = length;
  info.loop = loop;
  info.loopLength = loopLength;
  info.compType = 0; /* uncompressed PCM16 */
  info.extraData = NULL;

  hwSetTimeOffset(0);
  hwInitSamplePlayback((u32)voice, SMP_ID, &info, 1, 0, 0, 1, 0);
  hwSetPitch((u32)voice, 0x4000);
  hwSetVolume((u32)voice, 0, 1.f, 0x8000, 0, 0.f, 0.f);
  hwStart((u32)voice, TEST_STUDIO);
}

/*
 * A break on a looping voice must retire it within a bounded number of frames.
 * `breakOffset` is the subframe the break lands on (salTimeOffset), which is
 * what hwBreak uses to pick the changed[] slot.
 */
static void testBreakStopsLoopingVoice(const short* pcm, unsigned length, unsigned loop,
                                       unsigned loopLength, u8 breakOffset,
                                       const char* what) {
  const int voice = 1;
  int frames;

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(runUntilRunning(voice) > 0, what);
  if (dspVoice[voice].state != 2) {
    fprintf(stderr, "  (voice never reached the running state)\n");
    return;
  }

  hwSetTimeOffset(breakOffset);
  hwBreak(voice);
  hwSetTimeOffset(0);

  frames = runUntilStopped(voice);
  if (frames < 0) {
    fprintf(stderr, "FAIL: %s (still running after %d frames)\n", what, MAX_FRAMES);
    ++gFailures;
  } else {
    printf("  ok: %s stopped after %d frame(s)\n", what, frames);
  }
}

/*
 * A voice broken in the frame it started never plays. This is the startupBreak
 * rule, kept when the per-frame pass was added.
 */
static void testStartupBreakStillStopsNewVoice(const short* pcm, unsigned length, unsigned loop,
                                               unsigned loopLength) {
  const int voice = 2;
  static s16 dest[FRAME_SAMPLES * 2];

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(dspVoice[voice].state == 1, "startup-break: voice is new (state 1)");

  hwSetTimeOffset(0);
  hwBreak(voice);
  hwSetTimeOffset(0);
  check(dspVoice[voice].startupBreak != 0, "startup-break: flag raised");

  mixFrame(dest);
  check(dspVoice[voice].state == 0, "startup-break: never plays");
}

/*
 * A reused voice slot can carry a stale break bit in changed[0]: salActivateVoice
 * ORs 0x20 in when it takes a busy slot. That bit belongs to the slot's previous
 * occupant and must not kill the new voice.
 */
static void testStaleBreakBitDoesNotKillNewVoice(const short* pcm, unsigned length, unsigned loop,
                                                 unsigned loopLength) {
  const int voice = 3;

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(dspVoice[voice].state == 1, "stale-break: voice is new (state 1)");

  /* Exactly what salActivateVoice leaves behind when it steals a busy slot. */
  dspVoice[voice].changed[0] |= 0x20;
  check(dspVoice[voice].startupBreak == 0, "stale-break: no startupBreak");

  check(runUntilRunning(voice) > 0, "stale-break: new voice still starts");
}

/*
 * The other half of the steal rule: a busy slot really does get the stale bit, so
 * the case above is reachable rather than synthetic.
 */
static void testStealingBusyVoiceSetsStaleBreak(const short* pcm, unsigned length, unsigned loop,
                                                unsigned loopLength) {
  const int voice = 4;

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(runUntilRunning(voice) > 0, "steal: first voice runs");
  if (dspVoice[voice].state != 2) {
    return;
  }

  /* hwStart calls salActivateVoice, which sees state != 0 and marks the slot. */
  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(dspVoice[voice].state == 1, "steal: slot reused as a new voice");
  check((dspVoice[voice].changed[0] & 0x20) != 0, "steal: stale break bit present");
}

/* An unbroken looping voice must keep playing: the pass must not stop live sound. */
static void testUnbrokenLoopKeepsPlaying(const short* pcm, unsigned length, unsigned loop,
                                          unsigned loopLength) {
  const int voice = 5;
  static s16 dest[FRAME_SAMPLES * 2];
  int frames;
  int alive = 1;

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(runUntilRunning(voice) > 0, "unbroken loop: voice starts");
  if (dspVoice[voice].state != 2) {
    return;
  }

  /* Well past the frames a break needs to take effect. */
  for (frames = 0; frames < 16 && alive; ++frames) {
    mixFrame(dest);
    if (dspVoice[voice].state == 0) {
      alive = 0;
    }
  }
  check(alive, "unbroken loop: still running after 16 frames");
}

/* A voice holding a virtual sample is reported to the synth before retirement. */
static void testVirtualSampleNotified(const short* pcm, unsigned length, unsigned loop,
                                      unsigned loopLength) {
  const int voice = 6;

  gMessageCount = 0;
  gLastMessage = 0xffffffffu;

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(runUntilRunning(voice) > 0, "virtual sample: voice starts");
  if (dspVoice[voice].state != 2) {
    return;
  }

  /* What hw_dspctrl.c sets for a voice driving a virtual sample buffer. */
  dspVoice[voice].virtualSampleID = 0x1234;

  hwSetTimeOffset(0);
  hwBreak(voice);
  hwSetTimeOffset(0);

  check(runUntilStopped(voice) > 0, "virtual sample: voice stops");
  check(gMessageCount == 1, "virtual sample: synth notified exactly once");
  check(gLastMessage == 3, "virtual sample: message 3 (break)");
}

/* An ordinary voice owns no virtual sample, so nothing is reported. */
static void testOrdinaryVoiceNotNotified(const short* pcm, unsigned length, unsigned loop,
                                         unsigned loopLength) {
  const int voice = 7;

  gMessageCount = 0;
  gLastMessage = 0xffffffffu;

  startLoopingVoice(voice, pcm, length, loop, loopLength);
  check(runUntilRunning(voice) > 0, "ordinary voice: starts");
  if (dspVoice[voice].state != 2) {
    return;
  }

  check(dspVoice[voice].virtualSampleID == (u32)-1, "ordinary voice: owns no virtual sample");

  hwSetTimeOffset(0);
  hwBreak(voice);
  hwSetTimeOffset(0);

  check(runUntilStopped(voice) > 0, "ordinary voice: stops");
  check(gMessageCount == 0, "ordinary voice: synth not notified");
}

int main(void) {
  /* A short synthetic tone, looped over its second half. Loop length must be a
   * multiple of the 0x20-byte block the mixer reads, as a real sample is. */
  static short pcm[512];
  unsigned i;
  unsigned loop;
  unsigned loopLength;

  for (i = 0; i < 512; ++i) {
    pcm[i] = (short)(3000 * (((i % 32) < 16) ? 1 : -1));
  }
  loop = 256;
  loopLength = 256;

  /* The DSP allocates through the sound system's hooks. */
  {
    SND_HOOKS hooks;
    hooks.malloc = testMalloc;
    hooks.free = testFree;
    sndSetHooks(&hooks);
  }

  if (!salInitDspCtrl(TEST_VOICES, 1, 0)) {
    fprintf(stderr, "FAIL: salInitDspCtrl\n");
    return 1;
  }
  salActivateStudio(TEST_STUDIO, 1, SND_STUDIO_TYPE_STD);
  hwSetMesgCallback(onMessage);

  puts("charge hum: a break must stop a looping voice");

  testBreakStopsLoopingVoice(pcm, 512, loop, loopLength, 0, "break at subframe 0");
  testBreakStopsLoopingVoice(pcm, 512, loop, loopLength, 2, "break at a later subframe");
  testStartupBreakStillStopsNewVoice(pcm, 512, loop, loopLength);
  testStealingBusyVoiceSetsStaleBreak(pcm, 512, loop, loopLength);
  testStaleBreakBitDoesNotKillNewVoice(pcm, 512, loop, loopLength);
  testUnbrokenLoopKeepsPlaying(pcm, 512, loop, loopLength);
  testVirtualSampleNotified(pcm, 512, loop, loopLength);
  testOrdinaryVoiceNotNotified(pcm, 512, loop, loopLength);

  hwSetMesgCallback(NULL);
  salExitDspCtrl();

  if (gFailures != 0) {
    fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  puts("All charge-hum break checks passed");
  return 0;
}