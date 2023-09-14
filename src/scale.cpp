#include "scale.hpp"

chromaticNote middleC() {
  return chromaticNote { C, 4, MIDI_NOTE_MIDDLE_C };
}

float calculateChromaticVoltageSemitoneOffset(float volts) {
  return volts / CHROMATIC_SEMITONE_VOLTAGE;
}

float calculateChromaticNoteVoltage(chromaticNote note) {
  return (note.midi_note - middleC().midi_note) * CHROMATIC_SEMITONE_VOLTAGE;
}

chromaticNote chromaticNoteFromMidiNote(int midi_note) {
  chromaticNote note {
    (ChromaticName)(midi_note % TOTAL_CHROMATIC_NOTES),
    int(midi_note/TOTAL_CHROMATIC_NOTES) - 1,
    midi_note};
  return note;
}

chromaticNote chromaticNoteFromVoltage(float volts) {
  return chromaticNoteFromMidiNote(MIDI_NOTE_MIDDLE_C + std::round(calculateChromaticVoltageSemitoneOffset(volts)));
}

std::string chromaticNoteName(chromaticNote note) {
  switch (note.name) {
  case C:
    return "C_";
  case C_SHARP:
    return "C'";
  case D:
    return "D_";
  case D_SHARP:
    return "D'";
  case E:
    return "E_";
  case F:
    return "F_";
  case F_SHARP:
    return "F'";
  case G:
    return "G_";
  case G_SHARP:
    return "G'";
  case A:
    return "A_";
  case A_SHARP:
    return "A'";
  case B:
    return "B_";
  }
  return "xxx";
}

std::string chromaticNoteToString(chromaticNote note) {
  return chromaticNoteName(note) + std::to_string(note.octave);
}
