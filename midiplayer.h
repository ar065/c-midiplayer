// midi_player.h
#ifndef MIDI_PLAYER_H
#define MIDI_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Track data structure
typedef struct {
    uint8_t* data;
    uint8_t* long_msg;
    int tick;
    size_t offset, length;
    uint32_t message, temp;
    size_t long_msg_len;
    size_t long_msg_capacity;
    size_t data_capacity;
} TrackData;

// Callback function types
typedef void (*NoteOnCallback)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*NoteOffCallback)(uint8_t channel, uint8_t note);

// Public function
bool PlayMIDI(char* file, const NoteOnCallback note_on_callback, const NoteOffCallback note_off_callback);

#endif
