# JUCE patch: the Windows vblank thread with the display asleep (docs/05-m4-ui.md, 4h, idle CPU).
#
# With the display off, IDXGIOutput::WaitForVBlank returns at once, and JUCE's vblank thread
# then wakes the message thread about 750 times a second (its only brake is a 1 ms sleep), which
# costs ~2% of a core in any JUCE window while nothing is seen. After 8 waits in a row that return
# at once (under 0.2 ms), this paces the thread at about 60 Hz until a wait blocks again (the
# display back). A real vblank wait blocks until the next refresh, so a working display is never
# paced.
#
# Run as JUCE's patch step (CMakeLists.txt) in its source folder. It replaces whole one-line
# fragments, so it works on either line ending; it stops if JUCE changed (check the patch when
# upgrading) and does nothing if already applied. The file name and the cache key in CMakeLists.txt
# carry a version: a changed patch needs both raised, so JUCE is fetched and patched again.

set(file "modules/juce_gui_basics/native/juce_VBlank_windows.cpp")
file(READ "${file}" text)
if(text MATCHES "Lattice patch")
    return()
endif()

function(replace_once from to)
    string(FIND "${text}" "${from}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "JUCE vblank patch: '${from}' not found in ${file}; JUCE has changed, check cmake/patches")
    endif()
    string(REPLACE "${from}" "${to}" replaced "${text}")
    set(text "${replaced}" PARENT_SCOPE)
endfunction()

replace_once("            if (output->WaitForVBlank() == S_OK)"
"            const auto waitStart = Time::getMillisecondCounterHiRes(); // Lattice patch (cmake/patches)
            if (output->WaitForVBlank() == S_OK)")
replace_once("                if (now - lastVBlankEvent.exchange (now) < 1.0)"
"                // Lattice patch: with the display asleep the wait returns at once; after 8 such
                // waits in a row, pace at about 60 Hz until a wait blocks again.
                const auto sinceLast = now - lastVBlankEvent.exchange (now);
                immediateWaits = now - waitStart < 0.2 ? immediateWaits + 1 : 0;

                if (immediateWaits >= 8)
                    sleep (16);
                else if (sinceLast < 1.0)")
replace_once("    std::atomic<int> state{};"
"    std::atomic<int> state{};
    int immediateWaits = 0; // Lattice patch: waits in a row that returned at once (the vblank thread's)")

file(WRITE "${file}" "${text}")
message(STATUS "JUCE vblank patch applied")
