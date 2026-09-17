#pragma once

/*
** projectionscope.h
**
** A scope in which the game's own UI is run as a PASSIVE PROJECTION - laid out and drawn into an
** offscreen target for a second screen, an overlay, or a replay/spectator view - with no player
** having asked for it. Because nobody asked, nothing the projection does may be recorded.
**
** Selaco's PDA is what makes this concrete rather than theoretical: merely CONSTRUCTING the desktop
** reaches EventHandler.SendNetworkEvent("pdaUnreadClear") from PDAReaderWindow.init, which wipes the
** player's unread-datalog badges, and PDAAppWindow.layout records window geometry the same way on
** every layout. A projection has to be structurally incapable of those writes rather than carefully
** avoiding each one by hand, because the next app it hosts will have its own.
**
** SCOPE OF THE SUPPRESSION. Two things, for the same reason: a savegame write and a sound.
**
** 1. EventManager::SendNetworkEvent, the single funnel for both the ZScript
**    EventHandler.SendNetworkEvent native and the netevent CCMD. The sibling funnels
**    SendNetworkCommand and SendNetworkBuffer are deliberately NOT suppressed: nothing reachable
**    from the UI this was built for uses them, and an untested suppression is worse than an absent
**    one. Add them the same way if a future projection needs them.
**
** 2. The ZScript DObject.S_StartSound native, and only when the caller passes CHANF_UI. A sound is
**    as observable as a savegame write - more so, because the player hears it during gameplay
**    without ever having opened the UI that made it - so it belongs to this scope rather than to a
**    mechanism of its own. Selaco's PDA is again what makes it concrete: PDAMenu3.init plays
**    MenuSound("codex/open") (pda_menu.zs:90) and PDAAppWindow.onClose plays
**    Menu.MenuSound("codex/closeWindow") (app_window.zs:136), so a dashboard rebuilt on every
**    secret and every pickup chirps once per rebuild plus once per app it closes.
**
**    NOTE THAT THOSE TWO ARE NOT THE SAME CALL, which is why the guard is on the native and not on
**    the menu delegate. Selaco's UIMenu declares its own non-virtual menuSound(string, float)
**    (CockUI/menu.zs:848) that SHADOWS the engine's static Menu.MenuSound for every menu class
**    beneath it, so pda_menu.zs:90 never reaches menuDelegate at all; only the explicitly prefixed
**    Menu.MenuSound at app_window.zs:136 does. A third path, UIView.playSound (CockUI/view.zs:1327),
**    carries every button hover and click. All three end in the same S_StartSound native, which is
**    the only point at which one guard covers them.
**
** WHY CHANF_UI AND NOT EVERY SOUND, which is the one judgement call in here. The tempting argument for
** refusing all of them is that the scope only ever wraps our own synchronous VMCall, so nothing else can
** be running to have its sound swallowed. That argument does not hold, for two reasons.
**
** First, DObject.S_StartSound is NOT a UI-only native. It is declared static with flags defaulting to 0
** (doombase.zs:134), and Selaco's gameplay code calls it directly with no flags at all - the gravity
** rifle's telekinesis set (PLASMARIFLE_GRAV.zsc:38,51,95,147,427), a grate's melee impact
** (GENERALDECORATIONS.zsc:626), the player's armour-break and low-HP feedback
** (hud/hud_extension.zsc:149,392,499) and the gamepad ringer (AIMASSIST_HANDLER.zsc:73,104), about 17 of
** its 100 call sites in that tree. So a flagless guard here would not mean "refuse the projection's UI",
** it would mean "refuse whatever script is executing", which puts gameplay audio inside the blast radius
** of a scope whose entire purpose is to have none.
**
** Second, script we did not call CAN execute inside the scope. Allocating a DObject can run a GC step, and
** DestroyObjects (dobjgc.cpp:299) calls DObject::Destroy, which VM-dispatches OnDestroy
** (dobject.cpp:325-330) on objects that have nothing to do with the projection - and building the desktop
** allocates well over a hundred objects for that to happen in. Audited as this is written, none of the
** nine OnDestroy overrides in Selaco nor the eighteen in wadsrc reaches this native; the two that do play
** a sound (powerups.zs:1652,1738) use A_StartSound, which does not come through here. So refusing every
** sound would in fact be safe TODAY. The point of the flag test is that it stays safe without that audit
** being redone on every game patch - the same "structurally incapable rather than carefully avoiding each
** one by hand" property the top of this header claims for the netevent suppression.
**
** CHANF_UI is the engine's own name for the class of sound at issue - i_soundinternal.h:15 documents it as
** "Do not record sound in savegames", the same not-part-of-the-recorded-game boundary this whole header is
** about - and every sound Selaco's UI plays passes it: UIMenu.menuSound (CockUI/menu.zs:848),
** UIView.playSound (CockUI/view.zs:1327) and DoomMenuDelegate.PlaySound (doommenus.zs:69-72) are all
** S_StartSound(snd, CHAN_VOICE, CHANF_UI, snd_menuvolume). So the narrow predicate gives up nothing on the
** UI this was built for, and it fails in the direction where a mistake is audible rather than inaudible.
**
** WHAT THE FLAG TEST DOES NOT CATCH, so that it is not found as a surprise: a UI sound whose CHANF_UI
** landed in the wrong argument. hud_element_meters.zs:293,297 pass it as the CHANNEL, leaving flags 0, so
** those two would still play. They are HUD code and unreachable from a projection, but a PDA widget
** written the same way would leak one chirp.
**
** The sibling native DObject.S_Sound is NOT suppressed, on the same rule as the netevent funnels above: it
** is deprecated (doombase.zs:133) and has zero call sites in Selaco's tree. Actor sounds reach neither
** native - A_StartSound goes through S_PlaySoundPitch (s_doomsound.cpp:705) - which is also why the guard
** sits on the script native rather than on the shared S_SoundPitch helper it calls: S_PlaySoundPitch routes
** a CHANF_LOCAL actor sound through that same helper (:693), so guarding there would swallow gameplay audio
** outright.
**
** USE IT ONLY AROUND A SYNCHRONOUS VMCall ON THE GAME THREAD. The counters have no thread affinity
** because the things they guard have none either - SendNetworkEvent appends to the single local network
** buffer with Net_Write*, and the sound guard sits on a ZScript native, so both are reachable only from
** the VM, which runs on the game thread.
*/

// Depth-counted rather than a bool, so a nested projection cannot clear an outer one's suppression,
// and RAII because a VM abort unwinds as a C++ exception: a leaked flag would silently stop the
// PLAYER's own game from recording anything for the rest of the session.
class FProjectionScope
{
public:
	FProjectionScope() { ++Depth; }
	~FProjectionScope() { --Depth; }

	FProjectionScope(const FProjectionScope &) = delete;
	FProjectionScope &operator=(const FProjectionScope &) = delete;

	static bool Active() { return Depth > 0; }

	// Counts refusals, so a projection can report whether it actually suppressed anything instead of
	// leaving that invisible. Two counters rather than one total, because the two say different things
	// to whoever reads the log: Suppressed is savegame writes that would have corrupted player state,
	// SuppressedSounds is UI chirps the player would have heard. A projection that reports zero writes
	// and several sounds is working correctly; the reverse would be a new player-state write to go and
	// read. Neither is ever reset - each caller takes its own before/after delta.
	static int Suppressed;
	static int SuppressedSounds;

private:
	static int Depth;
};

inline int FProjectionScope::Depth = 0;
inline int FProjectionScope::Suppressed = 0;
inline int FProjectionScope::SuppressedSounds = 0;
