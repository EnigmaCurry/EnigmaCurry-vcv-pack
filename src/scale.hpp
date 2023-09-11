#include <string>
#include <math.h>
#include <format>
#include <iostream>

enum ChromaticName { C, C_SHARP, D, D_SHARP, E, F, F_SHARP, G, G_SHARP, A, A_SHARP, B, LAST_CHROMATIC_NOTE=B};
const int TOTAL_CHROMATIC_NOTES = ((int)LAST_CHROMATIC_NOTE) + 1;
const float CHROMATIC_SEMITONE_VOLTAGE = 1.0/TOTAL_CHROMATIC_NOTES;
const int MIDI_NOTE_MIDDLE_C = 60;

struct chromaticNote {
  ChromaticName name;
  int octave;
  int midi_note;
};

chromaticNote middleC();

float calculateChromaticVoltageSemitoneOffset(float volts);

chromaticNote chromaticNoteFromMidiNote(int midi_note);

chromaticNote chromaticNoteFromVoltage(float volts);

std::string chromaticNoteName(chromaticNote note);
std::string chromaticNoteToString(chromaticNote note);
