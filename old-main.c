#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "midiplayer.h"

#define NOTE_HEIGHT 6
#define MAX_KEYS 128
#define FLASH_DURATION 0.15f

#define RING_BUFFER_SIZE 134140400
typedef struct {
    uint8_t note;
    uint8_t velocity;
    bool isNoteOn;
    double timestamp;
} MidiEvent;

typedef struct {
    MidiEvent events[RING_BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
} EventQueue;

typedef struct {
    uint8_t note;
    double startTime;
    double endTime;
    uint8_t velocity;
    bool active;
    float flashTimer;
} NoteEvent;

typedef struct {
    NoteEvent* notes;
    int capacity;
    int count;
} NoteArray;

static EventQueue eventQueue = {0};
static NoteArray activeNotes = {0};
static double globalTime = 0.0;
static double timeOffset = 0.0;
static float scrollSpeed = 500.0f;
static RenderTexture2D pianoRollTexture;
static bool textureNeedsUpdate = true;
static int screenWidth = 1600;
static int screenHeight = 900;
static double lastRenderTime = 0.0;
static double renderInterval = 1.0 / 144.0;

// static uint64_t* notesPerSecond = 0;
static uint64_t notesPerSecond = 0;

static void init_event_queue() {
    eventQueue.head = 0;
    eventQueue.tail = 0;
    pthread_mutex_init(&eventQueue.mutex, NULL);
}

inline __attribute__((always_inline)) static bool queue_push(const MidiEvent event) {
    bool success = false;
    pthread_mutex_lock(&eventQueue.mutex);
    int next = (eventQueue.head + 1) % RING_BUFFER_SIZE;
    if (next != eventQueue.tail) {
        eventQueue.events[eventQueue.head] = event;
        eventQueue.head = next;
        success = true;
    }
    pthread_mutex_unlock(&eventQueue.mutex);
    return success;
}

inline __attribute__((always_inline)) static bool queue_pop(MidiEvent* event) {
    bool success = false;
    pthread_mutex_lock(&eventQueue.mutex);
    if (eventQueue.tail != eventQueue.head) {
        *event = eventQueue.events[eventQueue.tail];
        eventQueue.tail = (eventQueue.tail + 1) % RING_BUFFER_SIZE;
        success = true;
    }
    pthread_mutex_unlock(&eventQueue.mutex);
    return success;
}

inline __attribute__((always_inline)) static void init_note_array() {
    activeNotes.capacity = 1024;
    activeNotes.count = 0;
    activeNotes.notes = (NoteEvent*)malloc(sizeof(NoteEvent) * activeNotes.capacity);
}

inline __attribute__((always_inline)) static void ensure_note_capacity() {
    if (activeNotes.count >= activeNotes.capacity) {
        activeNotes.capacity *= 2;
        activeNotes.notes = (NoteEvent*)realloc(activeNotes.notes, sizeof(NoteEvent) * activeNotes.capacity);
    }
}

inline __attribute__((always_inline)) static float get_note_y_piano(const uint8_t note) {
    return screenHeight - ((float)(note + 1) / MAX_KEYS) * screenHeight;
}
inline __attribute__((always_inline)) static float get_note_y(const uint8_t note) {
    return ((float)(note + 1) / MAX_KEYS) * screenHeight;
}

inline __attribute__((always_inline)) static void note_on(uint8_t channel, const uint8_t note, const uint8_t velocity) {
    MidiEvent event = {
        .note = note,
        .velocity = velocity,
        .isNoteOn = true,
        .timestamp = GetTime() - timeOffset
    };
    queue_push(event);
}

inline __attribute__((always_inline)) static void note_off(uint8_t channel, const uint8_t note) {
    MidiEvent event = {
        .note = note,
        .velocity = 0,
        .isNoteOn = false,
        .timestamp = GetTime() - timeOffset
    };
    queue_push(event);
}

// static void notes_per_second(uint64_t* note_per_second)
// {
//     notesPerSecond = note_per_second;
// }
void notes_per_second(uint64_t nps) {
    notesPerSecond = nps;
    printf("Renderer got: %lu\n", nps);
}


inline __attribute__((always_inline)) static void process_midi_events() {
    MidiEvent event;
    while (queue_pop(&event)) {
        if (event.isNoteOn) {
            ensure_note_capacity();
            NoteEvent newNote = {
                .note = event.note,
                .startTime = event.timestamp,
                .endTime = -1.0,
                .velocity = event.velocity,
                .active = true,
                .flashTimer = FLASH_DURATION
            };
            activeNotes.notes[activeNotes.count++] = newNote;
            textureNeedsUpdate = true;
        } else {
            for (int i = activeNotes.count - 1; i >= 0; i--) {
                if (activeNotes.notes[i].note == event.note && activeNotes.notes[i].active && activeNotes.notes[i].endTime < 0.0) {
                    activeNotes.notes[i].endTime = event.timestamp;
                    activeNotes.notes[i].active = false;
                    textureNeedsUpdate = true;
                    break;
                }
            }
        }
    }
}


#define MAX_RENDERED_NOTES 300000

inline __attribute__((always_inline)) static void cleanup_notes() {
    const double cutoffTime = globalTime - (screenWidth / scrollSpeed);
    int writeIndex = 0;

    // First pass: remove notes that ended too early
    for (int i = 0; i < activeNotes.count; i++) {
        bool isOld = activeNotes.notes[i].endTime >= 0 && activeNotes.notes[i].endTime < cutoffTime;
        if (!isOld) {
            if (writeIndex != i) {
                activeNotes.notes[writeIndex] = activeNotes.notes[i];
            }
            writeIndex++;
        } else {
            textureNeedsUpdate = true;
        }
    }
    activeNotes.count = writeIndex;

    // Second pass: trim oldest notes if we exceed the max count
    if (activeNotes.count > MAX_RENDERED_NOTES) {
        int extra = activeNotes.count - MAX_RENDERED_NOTES;

        // Shift the remaining notes to remove the oldest ones
        memmove(activeNotes.notes, activeNotes.notes + extra, sizeof(NoteEvent) * (activeNotes.count - extra));
        activeNotes.count -= extra;
        textureNeedsUpdate = true;
    }
}

inline __attribute__((always_inline)) static void update_texture() {
    const double currentTime = GetTime() - timeOffset;
    if (!textureNeedsUpdate && (currentTime - lastRenderTime < renderInterval)) {
        return;
    }
    lastRenderTime = currentTime;
    BeginTextureMode(pianoRollTexture);
    ClearBackground(BLACK);

    for (int octave = 0; octave < 11; octave++) {
        int baseNote = octave * 12;
        Color lineColor = (Color){ 30, 30, 30, 255 };
        if (baseNote < MAX_KEYS) {
            float y = get_note_y(baseNote);
            DrawLine(0, y, screenWidth, y, lineColor);
        }
    }

    for (int i = 0; i < activeNotes.count; i++) {
        const NoteEvent* ev = &activeNotes.notes[i];
        const float duration = (ev->endTime >= 0.0) ? (ev->endTime - ev->startTime) : (globalTime - ev->startTime);
        const float x = (ev->startTime - (globalTime - screenWidth / scrollSpeed)) * scrollSpeed;
        const float width = duration * scrollSpeed;
        const float y = get_note_y(ev->note);
        if (x + width < 0 || x > screenWidth) continue;
        const float intensity = ev->velocity / 127.0f;
        const Color color = (Color){ (int)(intensity * 255), 64, 255, 255 };
        DrawRectangle(x, y - NOTE_HEIGHT, width, NOTE_HEIGHT, color);
    }
    EndTextureMode();
    textureNeedsUpdate = false;
}

static void* midi_thread(void* arg) {
    char* midiPath = (char*)arg;
    timeOffset = GetTime();
    PlayMIDI(midiPath, note_on, note_off, notes_per_second);
    return NULL;
}

int main(const int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <midi_file>\n", argv[0]);
        return 1;
    }

    init_event_queue();
    init_note_array();

    InitWindow(screenWidth, screenHeight, "Optimized MIDI Piano Roll");
    SetTargetFPS(144);
    pianoRollTexture = LoadRenderTexture(screenWidth, screenHeight);

    pthread_t midiThread;
    pthread_create(&midiThread, NULL, midi_thread, argv[1]);

    while (!WindowShouldClose()) {
        const double currentTime = GetTime() - timeOffset;
        const double deltaTime = currentTime - globalTime;
        globalTime = currentTime;

        textureNeedsUpdate = true;
        process_midi_events();

        // static double lastCleanup = 0;
        // if (globalTime - lastCleanup > 1.0) {
        //     cleanup_notes();
        //     lastCleanup = globalTime;
        // }
        cleanup_notes();


        update_texture();

        for (int i = 0; i < activeNotes.count; i++) {
            if (activeNotes.notes[i].flashTimer > 0.0f) {
                activeNotes.notes[i].flashTimer -= deltaTime;
            }
        }

        BeginDrawing();
        ClearBackground(BLACK);

        DrawTexture(pianoRollTexture.texture, 0, 0, WHITE);

        const int keyboardWidth = 40;
        DrawRectangle(screenWidth - keyboardWidth, 0, keyboardWidth, screenHeight, DARKGRAY);

        for (int note = 0; note < MAX_KEYS; note++) {
            const int noteType = note % 12;
            const float y = get_note_y(note);
            const bool isBlackKey = (noteType == 1 || noteType == 3 || noteType == 6 || noteType == 8 || noteType == 10);
            if (isBlackKey) {
                DrawRectangle(screenWidth - keyboardWidth/2, y - NOTE_HEIGHT, keyboardWidth/2, NOTE_HEIGHT, BLACK);
            }
            if (noteType == 0) {
                DrawText(TextFormat("C%d", note/12 - 1), screenWidth - keyboardWidth + 2, y - NOTE_HEIGHT - 8, 10, GRAY);
            }
        }

        for (int i = 0; i < activeNotes.count; i++) {
            const NoteEvent* ev = &activeNotes.notes[i];
            if (ev->flashTimer > 0.0f) {
                const float y = get_note_y_piano(ev->note);
                const float alpha = ev->flashTimer / FLASH_DURATION;
                Color flashColor = WHITE;
                flashColor.a = (unsigned char)(alpha * 255);
                DrawRectangle(screenWidth - keyboardWidth, y - NOTE_HEIGHT, keyboardWidth, NOTE_HEIGHT, flashColor);
            }
        }

        DrawLine(screenWidth - keyboardWidth, 0, screenWidth - keyboardWidth, screenHeight, WHITE);
        DrawFPS(10, 10);
        DrawText(TextFormat("Notes: %d", activeNotes.count), 10, 30, 20, GREEN);
        DrawText(TextFormat("NPS: %d", notesPerSecond), 10, 50, 20, SKYBLUE);

        EndDrawing();
    }

    UnloadRenderTexture(pianoRollTexture);
    free(activeNotes.notes);
    CloseWindow();
    return 0;
}