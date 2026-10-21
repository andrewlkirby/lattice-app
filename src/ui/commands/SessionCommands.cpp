#include "ui/commands/SessionCommands.h"

#include <memory>

namespace lattice::ui
{
namespace
{
using Kind = SessionSelection::Kind;
using juce::KeyPress;
using juce::ModifierKeys;

KeyPress ctrl(char c) { return KeyPress(c, ModifierKeys::commandModifier, 0); }
KeyPress ctrlShift(char c) { return KeyPress(c, ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 0); }
KeyPress plain(int keyCode) { return KeyPress(keyCode); }
KeyPress shift(int keyCode) { return KeyPress(keyCode, ModifierKeys::shiftModifier, 0); }

/** Runs f if the window provided it (otherwise the command is disabled and never runs). */
std::function<void()> call(std::function<void()> f)
{
    return [f = std::move(f)] {
        if (f)
            f();
    };
}

std::function<bool()> available(const std::function<void()>& f)
{
    const bool has = f != nullptr;
    return [has] { return has; };
}
} // namespace

void addSessionCommands(CommandRegistry& registry, SessionController& session, SelectionAccess selection,
                        ShellActions shell)
{
    auto& set = session.set();
    auto& undo = session.undoManager();
    const auto selected = [selection] { return selection.get(); };
    const auto clamp = [&set](SessionSelection s) {
        s.track = juce::jlimit(0, juce::jmax(0, set.numTracks() - 1), s.track);
        s.scene = juce::jlimit(0, juce::jmax(0, set.numScenes() - 1), s.scene);
        return s;
    };
    const auto hasClip = [&set, selected] {
        const auto s = selected();
        return s.kind == Kind::slot && set.clip(s.track, s.scene).isValid();
    };

    // --- Transport ------------------------------------------------------------------------------
    registry.add({.id = "transport.playStop",
                  .name = "Play/Stop",
                  .category = "Transport",
                  .description = "Starts the transport, or stops it if it's playing.",
                  .defaultKeys = {plain(KeyPress::spaceKey)},
                  .perform = [&session] { session.togglePlayStop(); },
                  .isTicked = [&session] { return session.transport().playing; }});
    registry.add({.id = "transport.play",
                  .name = "Play",
                  .category = "Transport",
                  .description = "Starts the transport from the beginning.",
                  .perform = [&session] { session.play(); },
                  .isTicked = [&session] { return session.transport().playing; }});
    registry.add({.id = "transport.stop",
                  .name = "Stop",
                  .category = "Transport",
                  .description = "Stops the transport and every clip; stopping again goes back to the beginning.",
                  .perform = [&session] { session.stop(); }});
    registry.add({.id = "transport.continue",
                  .name = "Continue",
                  .category = "Transport",
                  .description = "Starts the transport from where it stopped.",
                  .defaultKeys = {shift(KeyPress::spaceKey)},
                  .perform = [&session] { session.continuePlayback(); }});
    registry.add({.id = "transport.sessionRecord",
                  .name = "Session Record",
                  .category = "Transport",
                  .description = "Records into the selected scene's slots on armed tracks, or overdubs into clips playing on them.",
                  .defaultKeys = {plain(KeyPress::F9Key)},
                  .perform = [&session, selected] { session.setSessionRecord(!session.isSessionRecordOn(), selected().scene); },
                  .isTicked = [&session] { return session.isSessionRecordOn(); }});
    registry.add({.id = "transport.tapTempo",
                  .name = "Tap Tempo",
                  .category = "Transport",
                  .description = "Sets the tempo from the time between taps.",
                  .perform = [&session] { session.tapTempo(); }});
    registry.add({.id = "transport.metronome",
                  .name = "Metronome",
                  .category = "Transport",
                  .description = "Turns the metronome on or off.",
                  .perform = [&set] { set.setMetronomeOn(!set.metronomeOn()); },
                  .isTicked = [&set] { return set.metronomeOn(); },
                  .undoable = true});
    registry.add({.id = "transport.stopAllClips",
                  .name = "Stop All Clips",
                  .category = "Transport",
                  .description = "Stops every playing clip at the launch quantization.",
                  .perform = [&session] { session.stopAllClips(); }});

    // --- Edit -----------------------------------------------------------------------------------
    registry.add({.id = "edit.undo",
                  .name = "Undo",
                  .category = "Edit",
                  .description = "Undoes the last change.",
                  .defaultKeys = {ctrl('z')},
                  .perform = [&undo] { undo.undo(); },
                  .isEnabled = [&undo] { return undo.canUndo(); }});
    registry.add({.id = "edit.redo",
                  .name = "Redo",
                  .category = "Edit",
                  .description = "Redoes the last undone change.",
                  .defaultKeys = {ctrl('y'), ctrlShift('z')},
                  .perform = [&undo] { undo.redo(); },
                  .isEnabled = [&undo] { return undo.canRedo(); }});
    registry.add({.id = "edit.duplicate",
                  .name = "Duplicate",
                  .category = "Edit",
                  .description = "Duplicates the selected clip (into the next empty slot below), scene or track.",
                  .defaultKeys = {ctrl('d')},
                  .perform =
                      [&set, selection, clamp] {
                          const auto s = selection.get();
                          switch (s.kind)
                          {
                          case Kind::slot:
                              if (const int to = set.duplicateClip(s.track, s.scene); to >= 0)
                                  selection.set({Kind::slot, s.track, to});
                              break;
                          case Kind::scene:
                              selection.set(clamp({Kind::scene, s.track, set.duplicateScene(s.scene)}));
                              break;
                          case Kind::track:
                              selection.set(clamp({Kind::track, set.duplicateTrack(s.track), s.scene}));
                              break;
                          }
                      },
                  .isEnabled = [selected, hasClip] { return selected().kind != Kind::slot || hasClip(); },
                  .undoable = true});
    registry.add({.id = "edit.delete",
                  .name = "Delete",
                  .category = "Edit",
                  .description = "Deletes the selected clip, scene or track (a set always keeps one scene and one track).",
                  .defaultKeys = {plain(KeyPress::deleteKey), plain(KeyPress::backspaceKey)},
                  .perform =
                      [&set, selection, clamp] {
                          const auto s = selection.get();
                          switch (s.kind)
                          {
                          case Kind::slot:
                              set.deleteClip(s.track, s.scene);
                              break;
                          case Kind::scene:
                              set.deleteScene(s.scene);
                              break;
                          case Kind::track:
                              set.deleteTrack(s.track);
                              break;
                          }
                          selection.set(clamp(s));
                      },
                  .isEnabled =
                      [&set, selected, hasClip] {
                          const auto s = selected();
                          return s.kind == Kind::slot    ? hasClip()
                                 : s.kind == Kind::scene ? set.numScenes() > 1
                                                         : set.numTracks() > 1;
                      },
                  .undoable = true});
    registry.add({.id = "edit.rename",
                  .name = "Rename",
                  .category = "Edit",
                  .description = "Renames the selected clip, scene or track.",
                  .defaultKeys = {ctrl('r')},
                  .perform = call(shell.rename),
                  .isEnabled = [has = shell.rename != nullptr, selected, hasClip] {
                      return has && (selected().kind != Kind::slot || hasClip());
                  }});

    // --- File -----------------------------------------------------------------------------------
    registry.add({.id = "file.new",
                  .name = "New Set",
                  .category = "File",
                  .description = "Starts a new, empty set.",
                  .defaultKeys = {ctrl('n')},
                  .perform = call(shell.newSet),
                  .isEnabled = available(shell.newSet)});
    registry.add({.id = "file.open",
                  .name = "Open Set...",
                  .category = "File",
                  .description = "Opens a set from a file.",
                  .defaultKeys = {ctrl('o')},
                  .perform = call(shell.openSet),
                  .isEnabled = available(shell.openSet)});
    registry.add({.id = "file.save",
                  .name = "Save Set",
                  .category = "File",
                  .description = "Saves the set.",
                  .defaultKeys = {ctrl('s')},
                  .perform = call(shell.saveSet),
                  .isEnabled = available(shell.saveSet)});
    registry.add({.id = "file.saveAs",
                  .name = "Save Set As...",
                  .category = "File",
                  .description = "Saves the set to a new file.",
                  .defaultKeys = {ctrlShift('s')},
                  .perform = call(shell.saveSetAs),
                  .isEnabled = available(shell.saveSetAs)});
    registry.add({.id = "file.importMidi",
                  .name = "Import MIDI File...",
                  .category = "File",
                  .description = "Reads a MIDI file into clips, starting at the selected slot.",
                  .perform = call(shell.importMidi),
                  .isEnabled = [has = shell.importMidi != nullptr, selected] { return has && selected().kind == Kind::slot; }});

    // --- Create ---------------------------------------------------------------------------------
    registry.add({.id = "create.midiTrack",
                  .name = "Insert MIDI Track",
                  .category = "Create",
                  .description = "Inserts a MIDI track after the selected one.",
                  .defaultKeys = {ctrlShift('t')},
                  .perform =
                      [&set, selection] {
                          const auto s = selection.get();
                          selection.set({Kind::track, set.addTrack(s.track + 1), s.scene});
                      },
                  .undoable = true});
    registry.add({.id = "create.group",
                  .name = "Group Tracks",
                  .category = "Create",
                  .description = "Puts the selected track (with everything in it, for a group) into a new group track.",
                  .defaultKeys = {ctrl('g')},
                  .perform =
                      [&set, selection] {
                          const auto s = selection.get();
                          if (const int g = set.groupTracks(s.track, set.blockEnd(s.track) - 1); g >= 0)
                              selection.set({Kind::track, g, s.scene});
                      },
                  .isEnabled = [&set, selected] { return selected().kind != Kind::scene && selected().track < set.numTracks(); },
                  .undoable = true});
    registry.add({.id = "edit.ungroup",
                  .name = "Ungroup Tracks",
                  .category = "Edit",
                  .description = "Takes the tracks out of the selected group track, and deletes it.",
                  .defaultKeys = {ctrlShift('g')},
                  .perform =
                      [&set, selection] {
                          const auto s = selection.get();
                          set.ungroup(s.track);
                          selection.set({Kind::track, juce::jmin(s.track, set.numTracks() - 1), s.scene});
                      },
                  .isEnabled = [&set, selected] {
                      const auto s = selected();
                      return s.kind != Kind::scene && s.track < set.numTracks() && set.isGroupTrack(s.track);
                  },
                  .undoable = true});
    registry.add({.id = "create.scene",
                  .name = "Insert Scene",
                  .category = "Create",
                  .description = "Inserts a scene after the selected one.",
                  .defaultKeys = {ctrl('i')},
                  .perform =
                      [&set, selection] {
                          const auto s = selection.get();
                          selection.set({Kind::scene, s.track, set.addScene(s.scene + 1)});
                      },
                  .undoable = true});
    registry.add({.id = "create.midiClip",
                  .name = "Insert MIDI Clip",
                  .category = "Create",
                  .description = "Creates an empty one-bar clip in the selected slot.",
                  .defaultKeys = {ctrlShift('m')},
                  .perform =
                      [&set, selected] {
                          const auto s = selected();
                          set.createClip(s.track, s.scene, set.beatsPerBar());
                      },
                  .isEnabled = [selected, hasClip] { return selected().kind == Kind::slot && !hasClip(); },
                  .undoable = true});

    // --- Session --------------------------------------------------------------------------------
    // Enter launches; a Gate clip plays while it's held, so the release goes to the slot it launched.
    auto held = std::make_shared<SessionSelection>();
    auto holding = std::make_shared<bool>(false);
    registry.add({.id = "session.launch",
                  .name = "Launch",
                  .category = "Session",
                  .description = "Launches the selected clip or scene (a Gate clip plays while the key is held).",
                  .defaultKeys = {plain(KeyPress::returnKey)},
                  .perform =
                      [&session, &set, selection, clamp, held, holding] {
                          const auto s = selection.get();
                          if (s.kind == Kind::scene)
                          {
                              session.launchScene(s.scene);
                              if (set.selectNextSceneOnLaunch())
                                  selection.set(clamp({Kind::scene, s.track, s.scene + 1}));
                          }
                          else if (!*holding)
                          {
                              session.launchSlot(s.track, s.scene);
                              *held = s;
                              *holding = true;
                          }
                      },
                  .onKeyUp =
                      [&session, held, holding] {
                          if (*holding)
                              session.releaseSlot(held->track, held->scene);
                          *holding = false;
                      }});
    registry.add({.id = "session.stopTrack",
                  .name = "Stop Track",
                  .category = "Session",
                  .description = "Stops the selected track's clips.",
                  .perform = [&session, selected] { session.stopTrack(selected().track); }});

    const auto move = [selection, clamp](int dx, int dy) {
        auto s = selection.get();
        if (s.kind == Kind::track && dy > 0)
            s.kind = Kind::slot; // down from a header goes into its slots
        s.track += dx;
        s.scene += dy;
        selection.set(clamp(s));
    };
    const struct
    {
        const char* id;
        const char* name;
        const char* description;
        int key, dx, dy;
    } moves[] = {
        {"session.selectUp", "Select Up", "Moves the selection up a scene.", KeyPress::upKey, 0, -1},
        {"session.selectDown", "Select Down", "Moves the selection down a scene.", KeyPress::downKey, 0, 1},
        {"session.selectLeft", "Select Left", "Moves the selection to the previous track.", KeyPress::leftKey, -1, 0},
        {"session.selectRight", "Select Right", "Moves the selection to the next track.", KeyPress::rightKey, 1, 0},
    };
    for (const auto& m : moves)
        registry.add({.id = m.id,
                      .name = m.name,
                      .category = "Session",
                      .description = m.description,
                      .defaultKeys = {plain(m.key)},
                      .perform = [move, dx = m.dx, dy = m.dy] { move(dx, dy); }});
}

} // namespace lattice::ui
